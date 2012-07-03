/*
 * opencog/learning/moses/moses/scoring.cc
 *
 * Copyright (C) 2002-2008 Novamente LLC
 * Copyright (C) 2012 Poulin Holdings
 * All Rights Reserved
 *
 * Written by Moshe Looks, Nil Geisweiller, Linas Vepstas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "scoring.h"

#include <cmath>

#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm_ext/for_each.hpp>
#include <boost/range/algorithm/transform.hpp>
#include <boost/range/algorithm/max_element.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>

#include <opencog/util/numeric.h>
#include <opencog/util/KLD.h>
#include <opencog/util/MannWhitneyU.h>

namespace opencog { namespace moses {

using namespace std;
using boost::adaptors::map_values;
using boost::adaptors::map_keys;
using boost::adaptors::filtered;
using boost::adaptors::transformed;
using boost::transform;
using namespace boost::phoenix;
using boost::phoenix::arg_names::arg1;
using namespace boost::accumulators;

// helper to log a combo_tree and its behavioral score
inline void log_candidate_pbscore(const combo_tree& tr,
                                  const penalized_behavioral_score& pbs)
{
    if (!logger().isFineEnabled())
        return;

    logger().fine() << "Evaluate candidate: " << tr << "\n"
                    << "\tBScored: " << pbs;
}

void bscore_base::set_complexity_coef(unsigned alphabet_size, float p)
{
    // Both p==0.0 and p==0.5 are singularities in the forumla.
    // See the explanation in the comment above ctruth_table_bscore.
    complexity_coef = 0.0;
    occam = (p > 0.0f && p < 0.5f);
    if (occam)
        complexity_coef = discrete_complexity_coef(alphabet_size, p);

    logger().info() << "BScore noise = " << p
                    << " alphabest size = " << alphabet_size
                    << " complexity ratio = " << 1.0/complexity_coef;
}

void bscore_base::set_complexity_coef(score_t complexity_ratio)
{
    complexity_coef = 0.0;
    occam = (complexity_ratio > 0.0);
    if (occam)
        complexity_coef = 1.0 / complexity_ratio;

    logger().info() << "BScore complexity ratio = " << 1.0/complexity_coef;
}

////////////////////
// logical_bscore //
////////////////////
        
penalized_behavioral_score logical_bscore::operator()(const combo_tree& tr) const
{
    combo::complete_truth_table tt(tr, arity);
    penalized_behavioral_score pbs(
        make_pair<behavioral_score, score_t>(behavioral_score(target.size()), 0));

    boost::transform(tt, target, pbs.first.begin(), [](bool b1, bool b2) {
            return -score_t(b1 != b2); });

    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    return pbs;
}

behavioral_score logical_bscore::best_possible_bscore() const
{
    return behavioral_score(target.size(), 0);
}

score_t logical_bscore::min_improv() const
{
    return 0.5;
}

///////////////////
// contin_bscore //
///////////////////

// Note that this returns a POSITIVE number.
score_t contin_complexity_coef(unsigned alphabet_size, double stdev)
{
    return log(alphabet_size) * 2 * sq(stdev);
}

penalized_behavioral_score contin_bscore::operator()(const combo_tree& tr) const
{
    // OTable target is the table of output we want to get.
    penalized_behavioral_score pbs;

    // boost/range/algorithm/transform.
    // Take the input vectors cit, target, feed the elts to anon
    // funtion[] (which just computes square of the difference) and
    // put the results into bs.
    boost::transform(cti, target, back_inserter(pbs.first),
                     [&](const vertex_seq& vs, const vertex& v) {
                         contin_t tar = get_contin(v),
                             res = get_contin(eval_binding(vs, tr));
                         return -err_func(res, tar);
                     });
    // add the Occam's razor feature
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    // Logger
    log_candidate_pbscore(tr, pbs);
    // ~Logger

    return pbs;
}

behavioral_score contin_bscore::best_possible_bscore() const
{
    return behavioral_score(target.size(), 0);
}

score_t contin_bscore::min_improv() const
{
    // The backwards compat version of this is 0.0.  But for
    // continuously-variable scores, this is crazy, as the
    // system falls into a state of tweaking the tenth decimal place,
    // Limit any such tweaking to 4 decimal places of precision.
    // (thus 1e-4 below).
    //
    // Note: positive min_improv is taken as an absolute score.
    // Negative min_improve is treated as a relative score.
    return -1.0e-4;
}
        
void contin_bscore::set_complexity_coef(unsigned alphabet_size, float stdev)
{
    occam = (stdev > 0.0);
    complexity_coef = 0.0;
    if (occam)
        complexity_coef = contin_complexity_coef(alphabet_size, stdev);

    logger().info() << "contin_bscore noise = " << stdev
                    << " alphabest size = " << alphabet_size
                    << " complexity ratio = " << 1.0/complexity_coef;
}

///////////////////
// discriminator //
///////////////////

discriminator::discriminator(const CTable& ct)
    : _ctable(ct)
{
    _output_type = get_type_node(get_signature_output(ct.tt));
    if (_output_type == id::boolean_type) {
        // For boolean tables, sum the total number of 'T' values
        // in the output. 
        sum_outputs = [](const CTable::counter_t& c)->score_t
        {
            return c.get(id::logical_true);
        };
    } else if (_output_type == id::contin_type) {
        // For contin tables, we return the sum of the row values.
        sum_outputs = [](const CTable::counter_t& c)->score_t
        {
            score_t res = 0.0;
            foreach(const CTable::counter_t::value_type& cv, c)
                res += get_contin(cv.first) * cv.second;
            return res;
        };
    } else {
        OC_ASSERT(false, "Discriminator, unsupported output type");
        return;
    }

    _positive_total = 0.0;
    _negative_total = 0.0;
    foreach(const CTable::value_type& vct, _ctable) {
        // vct.first = input vector
        // vct.second = counter of outputs

        contin_t sum_pos = sum_outputs(vct.second);
        contin_t sum_neg;
        unsigned totalc = vct.second.total_count();
        if (_output_type == id::boolean_type) {
            sum_neg = totalc - sum_pos;
        } else {
            sum_neg = -sum_pos;
        }
        _positive_total += sum_pos;
        _negative_total += sum_neg;
    }
    logger().info() << "Discriminator: num_positive=" << _positive_total
                    << " num_negative=" << _negative_total;
}


discriminator::d_counts::d_counts()
{
    true_positive_sum = 0.0;
    false_positive_sum = 0.0;
    positive_count = 0.0;
    true_negative_sum = 0.0;
    false_negative_sum = 0.0;
    negative_count = 0.0;
};

discriminator::d_counts discriminator::count(const combo_tree& tr) const
{
    d_counts ctr;

    foreach(const CTable::value_type& vct, _ctable) {
        // vct.first = input vector
        // vct.second = counter of outputs

        contin_t sum_pos = sum_outputs(vct.second);
        contin_t sum_neg;
        unsigned totalc = vct.second.total_count();
        if (_output_type == id::boolean_type) {
            sum_neg = totalc - sum_pos;
        } else {
            sum_neg = -sum_pos;
        }

        if (eval_binding(vct.first, tr) == id::logical_true)
        {
            ctr.true_positive_sum += sum_pos;
            ctr.false_positive_sum += sum_neg;
            ctr.positive_count += totalc;
        }
        else
        {
            ctr.true_negative_sum += sum_neg;
            ctr.false_negative_sum += sum_pos;
            ctr.negative_count += totalc;
        }
    }
    return ctr;
}

//////////////////////////
// disciminating_bscore //
/////////////////////////

discriminating_bscore::discriminating_bscore(const CTable& ct,
                  float min_threshold,
                  float max_threshold,
                  float hardness) 
    : discriminator(ct),
    _ctable_usize(ct.uncompressed_size()),
    _min_threshold(min_threshold),
    _max_threshold(max_threshold),
    _hardness(hardness)
{
    logger().info("Discriminating scorer, hardness = %f, "
                  "min_threshold = %f, "
                  "max_threshold = %f",
                  _hardness, _min_threshold, _max_threshold);

    // Verify that the thresholds are sane
    OC_ASSERT((0.0 < hardness) && (0.0 < min_threshold) && (min_threshold <= max_threshold),
        "Discriminating scorer, invalid thresholds.  "
        "The hardness must be positive, the minimum threshold must be "
        "greater than zero, and the maximum threshold must be greater "
        "than or equal to the minimum threshold.\n");

    // For boolean tables, the highest possible output is 1.0 (of course)
    if (_output_type == id::boolean_type) {
        _max_output = 1.0;
        _min_output = 0.0;
    }
    else // if (_output_type == id::contin_type)
    {
        // For contin tables, we search for the largest value in the table.
        _max_output = worst_score;
        _min_output = -worst_score;
        foreach(const auto& cr, _ctable) {
            const CTable::counter_t& c = cr.second;
            foreach(const auto& cv, c) {
                score_t val = get_contin(cv.first);
                _max_output = std::max(_max_output, val);
                _min_output = std::min(_min_output, val);
            }
        }
    }

    logger().info("Discriminating scorer, min_output = %f, "
                  "max_output = %f", _min_output, _max_output);
}

behavioral_score discriminating_bscore::best_possible_bscore() const
{
    // create a list, maintained in sorted order.
    typedef std::multimap<contin_t, std::pair<contin_t, // variable
                                              contin_t> // fixed
                          > max_vary_t;
    max_vary_t max_vary;
    for (CTable::const_iterator it = _ctable.begin(); it != _ctable.end(); ++it)
    {
        const CTable::counter_t& c = it->second;

        unsigned total = c.total_count();
        contin_t sum_pos = sum_outputs(c);
        contin_t sum_neg;
        if (_output_type == id::boolean_type) {
            sum_neg = total - sum_pos;
        } else {
            sum_neg = -sum_pos;
        }

        contin_t vary = get_variable(sum_pos, sum_neg, total);
        contin_t fix = get_fixed(sum_pos, sum_neg, total);
        auto lmnt = std::make_pair(vary, std::make_pair(vary, fix));
        max_vary.insert(lmnt);
    }

    // Sum up the best score, until the minimum fixed threshold is
    // reached.  It's not clear this actually gives the best score one
    // can get if min_threshold isn't reached, but we don't want to go
    // below min_threshold anyway, so it's an acceptable inacurracy.
    // (It would be a problem only if the threshold constraint is very loose.)
    //
    score_t fix_sum = 0;
    score_t best_score = 0.0;
    reverse_foreach (const auto& mpv, max_vary) {
        best_score += mpv.second.first;
        fix_sum += mpv.second.second;
        if (_min_threshold <= fix_sum)
            break;
    }

    score_t fixation_penalty = get_threshold_penalty(fix_sum);

    logger().info("Discriminating scorer, score at threshold = %f", best_score);
    logger().info("Discriminating scorer, fixed component at threshold = %f", fix_sum);
    logger().info("Discriminating scorer, fixation penalty at threshold = %f", fixation_penalty);

    return {best_score, fixation_penalty};
}

score_t discriminating_bscore::min_improv() const
{
    return 1.0 / _ctable_usize;
}

// Note that the logarithm is always negative, so this method always
// returns a value that is zero or negative.
score_t discriminating_bscore::get_threshold_penalty(score_t value) const
{
    score_t dst = 0.0;
    if (value < _min_threshold)
        dst = 1.0 - value / _min_threshold;
    
    if (_max_threshold < value)
        dst = (value - _max_threshold) / (1.0 - _max_threshold);

    return _hardness * log(1.0 - dst);
}

void discriminating_bscore::set_complexity_coef(unsigned alphabet_size, float p)
{
    complexity_coef = 0.0;
    // Both p==0.0 and p==0.5 are singularity points in the Occam's
    // razor formula for discrete outputs (see the explanation in the
    // comment above ctruth_table_bscore)
    occam = p > 0.0f && p < 0.5f;
    if (occam)
        complexity_coef = discrete_complexity_coef(alphabet_size, p)
            / _ctable_usize;     // normalized by the size of the table
                                // because the precision is normalized
                                // as well

    logger().info() << "Discriminiating scorer, noise = " << p
                    << " alphabest size = " << alphabet_size
                    << " complexity ratio = " << 1.0/complexity_coef;
}

void discriminating_bscore::set_complexity_coef(score_t ratio)
{
    complexity_coef = 0.0;
    occam = (ratio > 0);

    // The complexity coeff is normalized by the size of the table,
    // because the precision is normalized as well.  So e.g.
    // max precision for boolean problems is 1.0.  However...
    // umm XXX I think the normalization here should be the
    // best-possible activation, not the usize, right?
    //
    // @todo Sounds good too, as long as it's constant, so you would
    // replace _ctable_usize by _ctable_usize * max_activation?
    if (occam)
        complexity_coef = 1.0 / (_ctable_usize * ratio);

    logger().info() << "Discriminating scorer, complexity ratio = " << 1.0f/complexity_coef;
}


///////////////////
// recall_bscore //
///////////////////

recall_bscore::recall_bscore(const CTable& ct,
                  float min_precision,
                  float max_precision,
                  float hardness) 
    : discriminating_bscore(ct, min_precision, max_precision, hardness)
{
}

penalized_behavioral_score recall_bscore::operator()(const combo_tree& tr) const
{
    d_counts ctr = count(tr);

    // Compute normalized precision and recall.
    score_t precision = ctr.true_positive_sum / (ctr.true_positive_sum + ctr.false_positive_sum);
    score_t recall = ctr.true_positive_sum / (ctr.true_positive_sum + ctr.false_negative_sum);

    // We are maximizing recall, so that is the first part of the score.
    penalized_behavioral_score pbs;
    pbs.first.push_back(recall);
    
    score_t precision_penalty = get_threshold_penalty(precision);
    pbs.first.push_back(precision_penalty);
    if (logger().isFineEnabled()) 
        logger().fine("precision = %f  recall=%f  precision penalty=%e",
                     precision, recall, precision_penalty);
 
    // Add the Complexity penalty
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

/// Return the precision for this ctable row.
score_t recall_bscore::get_fixed(score_t pos, score_t neg, unsigned cnt) const
{
    contin_t precision = pos / (cnt * _positive_total);
    return precision;
}

/// Return the recall for this ctable row.
/// XXX I think this is correct, double check... TODO.
score_t recall_bscore::get_variable(score_t pos, score_t neg, unsigned cnt) const
{
    contin_t recall = 1.0 / _ctable_usize;
    return recall;
}

///////////////////
// prerec_bscore //
///////////////////

prerec_bscore::prerec_bscore(const CTable& ct,
                  float min_recall,
                  float max_recall,
                  float hardness) 
    : discriminating_bscore(ct, min_recall, max_recall, hardness)
{
}

// Nearly identical to recall_bscore, except that the roles of precision
// and recall are switched.
penalized_behavioral_score prerec_bscore::operator()(const combo_tree& tr) const
{
    d_counts ctr = count(tr);

    // Compute normalized precision and recall.
    score_t precision = ctr.true_positive_sum / (ctr.true_positive_sum + ctr.false_positive_sum);
    score_t recall = ctr.true_positive_sum / (ctr.true_positive_sum + ctr.false_negative_sum);

    // We are maximizing recall, so that is the first part of the score.
    penalized_behavioral_score pbs;
    pbs.first.push_back(precision);
    
    score_t recall_penalty = get_threshold_penalty(recall);
    pbs.first.push_back(recall_penalty);
    if (logger().isFineEnabled()) 
        logger().fine("precision = %f  recall=%f  recall penalty=%e",
                     precision, recall, recall_penalty);
 
    // Add the Complexity penalty
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

/// Return the precision for this ctable row.
score_t prerec_bscore::get_variable(score_t pos, score_t neg, unsigned cnt) const
{
    contin_t precision = pos / (cnt * _positive_total);
    return precision;
}

/// Return the recall for this ctable row.
/// XXX I think this is correct, double check... TODO.
score_t prerec_bscore::get_fixed(score_t pos, score_t neg, unsigned cnt) const
{
    contin_t recall = 1.0 / _ctable_usize;
    return recall;
}

//////////////////////
// precision_bscore //
//////////////////////

precision_bscore::precision_bscore(const CTable& _ctable,
                                   float penalty_,
                                   float min_activation_,
                                   float max_activation_,
                                   bool positive_,
                                   bool worst_norm_)
    : ctable(_ctable), ctable_usize(ctable.uncompressed_size()),
      min_activation(min_activation_), max_activation(max_activation_),
      penalty(penalty_), positive(positive_), worst_norm(worst_norm_)
{
    output_type = get_type_node(get_signature_output(ctable.tt));
    if (output_type == id::boolean_type) {
        // For boolean tables, sum the total number of 'T' values
        // in the output.  Ths sum represents the best possible score
        // i.e. we found all of the true values correcty.  Count
        // 'F' is 'positive' is false.
        vertex target = bool_to_vertex(positive);
        sum_outputs = [target](const CTable::counter_t& c)->score_t
        {
            return c.get(target);
        };
    } else if (output_type == id::contin_type) {
        // For contin tables, we return the sum of the row values.
        sum_outputs = [this](const CTable::counter_t& c)->score_t
        {
            score_t res = 0.0;
            foreach(const CTable::counter_t::value_type& cv, c)
                res += get_contin(cv.first) * cv.second;
            return (this->positive? res : -res);
        };
    } else {
        OC_ASSERT(false, "Precision scorer, unsupported output type");
        return;
    }

    logger().info("Precision scorer, penalty = %f, "
                  "min_activation = %f, "
                  "max_activation = %f",
                  penalty, min_activation, max_activation);

    // Verify that the penaly is sane
    OC_ASSERT((0.0 < penalty) && (0.0 < min_activation) && (min_activation <= max_activation),
        "Precision scorer, invalid activation bounds.  "
        "The penalty must be non-zero, the minimum activation must be "
        "greater than zero, and the maximum activation must be greater "
        "than or equal to the minimum activation.\n");

    // For boolean tables, the highest possible precision is 1.0 (of course)
    if (output_type == id::boolean_type)
        max_output = 1.0;
    else if (output_type == id::contin_type) {

        // For contin tables, we search for the largest value in the table.
        // (or smallest, if positive == false)
        max_output = worst_score;
        foreach(const auto& cr, ctable) {
            const CTable::counter_t& c = cr.second;
            foreach(const auto& cv, c) {
                score_t val = get_contin(cv.first);
                if (!positive) val = -val;
                max_output = std::max(max_output, val);
            }
        }
    }

    logger().info("Precision scorer, max_output = %f", max_output);
}

void precision_bscore::set_complexity_coef(unsigned alphabet_size, float p)
{
    complexity_coef = 0.0;
    // Both p==0.0 and p==0.5 are singularity points in the Occam's
    // razor formula for discrete outputs (see the explanation in the
    // comment above ctruth_table_bscore)
    occam = p > 0.0f && p < 0.5f;
    if (occam)
        complexity_coef = discrete_complexity_coef(alphabet_size, p)
            / ctable_usize;     // normalized by the size of the table
                                // because the precision is normalized
                                // as well

    logger().info() << "Precision scorer, noise = " << p
                    << " alphabest size = " << alphabet_size
                    << " complexity ratio = " << 1.0/complexity_coef;
}

void precision_bscore::set_complexity_coef(score_t ratio)
{
    complexity_coef = 0.0;
    occam = (ratio > 0);

    // The complexity coeff is normalized by the size of the table,
    // because the precision is normalized as well.  So e.g.
    // max precision for boolean problems is 1.0.  However...
    // umm XXX I think the normalization here should be the
    // best-possible activation, not the usize, right?
    if (occam)
        complexity_coef = 1.0 / (ctable_usize * ratio);

    logger().info() << "Precision scorer, complexity ratio = " << 1.0f/complexity_coef;
}

penalized_behavioral_score precision_bscore::operator()(const combo_tree& tr) const
{
    penalized_behavioral_score pbs;

    // associate sum of worst outputs with number of observations for
    // that sum
    multimap<contin_t, unsigned> worst_deciles;

    // compute active and sum of all active outputs
    unsigned active = 0;   // total number of active outputs by tr
    score_t sao = 0.0;     // sum of all active outputs (in the boolean case)
    foreach(const CTable::value_type& vct, ctable) {
        // vct.first = input vector
        // vct.second = counter of outputs
        if (eval_binding(vct.first, tr) == id::logical_true) {
            contin_t sumo = sum_outputs(vct.second);
            unsigned totalc = vct.second.total_count();
            // For boolean tables, sao == sum of all true positives,
            // and active == sum of true+false positives.
            // For contin tables, sao = sum of contin values, and
            // active == count of rows.
            sao += sumo;
            active += totalc;
            if (worst_norm && sumo < 0)
                worst_deciles.insert({sumo, totalc});
        }
    }

    // remove all observations from worst_norm so that only the worst
    // n_deciles or less remains and compute its average
    contin_t avg_worst_deciles = 0.0;
    if (worst_norm and sao > 0) {
        unsigned worst_count = 0,
            n_deciles = active / 10;
        foreach (const auto& pr, worst_deciles) {
            worst_count += pr.second;
            avg_worst_deciles += pr.first;
            if (worst_count > n_deciles)
                break;
        }
        avg_worst_deciles /= worst_count;
    }

    // Compute normalized precision.  No hits means perfect precision :)
    // Yes, zero hits is common, early on.
    score_t precision = 1.0;
    if (0 < active)
        precision = (sao / active) / max_output;

    // normalize precision w.r.t. worst deciles
    if (avg_worst_deciles < 0) {
        logger().fine("precision before worst_norm = %f", precision);
        logger().fine("abs(avg_worst_deciles) = %f", -avg_worst_deciles);
        precision /= -avg_worst_deciles;
        if (avg_worst_deciles >= 0)
            logger().fine("Weird: worst_norm (%f) is positive, maybe the activation is really low", avg_worst_deciles);
    }

    pbs.first.push_back(precision);

    // For boolean tables, activation sum of true and false positives
    // i.e. the sum of all positives.   For contin tables, the activation
    // is likewise: the number of rows for which the combo tree returned
    // true (positive).
    score_t activation = (score_t)active / ctable_usize;
    score_t activation_penalty = get_activation_penalty(activation);
    pbs.first.push_back(activation_penalty);
    if (logger().isFineEnabled())
        logger().fine("precision = %f  activation=%f  activation penalty=%e",
                     precision, activation, activation_penalty);

    // Add the Complexity penalty
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

behavioral_score precision_bscore::best_possible_bscore() const
{
    // @todo doesn't treat the case with worst_norm

    // For each row, compute the maximum precision it can get.
    // Typically, this is 0 or 1 for nondegenerate boolean tables.
    // Also store the sumo and total, so that they don't need to be
    // recomputed later.  Note that this routine could be performance
    // critical if used as a fitness function for feature selection
    // (which is planned).
    typedef std::multimap<contin_t, std::pair<contin_t, // sum_outputs
                                              unsigned> // total count
                          > max_precisions_t;
    max_precisions_t max_precisions;
    for (CTable::const_iterator it = ctable.begin(); it != ctable.end(); ++it) {
        const CTable::counter_t& c = it->second;
        contin_t sumo = sum_outputs(c);
        unsigned total = c.total_count();
        contin_t precision = sumo / total;
        auto lmnt = std::make_pair(precision, std::make_pair(sumo, total));
        max_precisions.insert(lmnt);
    }

    // Compute best precision till minimum activation is reached. Note
    // that the best precision (sao / active) can never increase for each
    // new mpv.  Despite this, we keep going until at least min_activation
    // is reached. It's not clear this actually gives the best score one
    // can get if min_activation isn't reached, but we don't want to go
    // below min activation anyway, so it's an acceptable inacurracy.
    // (It would be a problem only if activation constraint is very loose.)
    //
    unsigned active = 0;
    score_t sao = 0.0;
    reverse_foreach (const auto& mpv, max_precisions) {
        sao += mpv.second.first;
        active += mpv.second.second;
        if (ctable_usize * min_activation <= active)
            break;
    }

    score_t precision = (sao / active) / max_output;

    score_t activation = active / (score_t)ctable_usize;
    score_t activation_penalty = get_activation_penalty(activation);

    logger().info("Precision scorer, precision at min activation = %f", precision);
    logger().info("Precision scorer, activation at above precision = %f", activation);
    logger().info("Precision scorer, activation penalty at above precision = %f", activation_penalty);

    return {precision, activation_penalty};
}

// Note that the logarithm is always negative, so this method always
// returns a value that is zero or negative.
score_t precision_bscore::get_activation_penalty(score_t activation) const
{
    score_t dst = 0.0;
    if (activation < min_activation)
        dst = 1.0 - activation/min_activation;

    if (max_activation < activation)
        dst = (activation - max_activation) / (1.0 - max_activation);

    // logger().fine("activation penalty = %f", dst);
    return penalty * log(1.0 - dst);
}

score_t precision_bscore::min_improv() const
{
    return 1.0 / ctable_usize;
}

combo_tree precision_bscore::gen_canonical_best_candidate() const
{
    // @todo doesn't treat the case with worst_norm

    // For each row, compute the maximum precision it can get.
    // Typically, this is 0 or 1 for nondegenerate boolean tables.
    // Also store the sumo and total, so that they don't need to be
    // recomputed later.  Note that this routine could be performance
    // critical if used as a fitness function for feature selection
    // (which is planned).
    typedef std::multimap<contin_t, // precision
                          std::pair<CTable::const_iterator,
                                    unsigned> // total count
                          > precision_to_count_t;
    precision_to_count_t ptc;
    for (CTable::const_iterator it = ctable.begin(); it != ctable.end(); ++it) {
        const CTable::counter_t& c = it->second;
        unsigned total = c.total_count();
        contin_t precision = sum_outputs(c) / total;
        ptc.insert(std::make_pair(precision, std::make_pair(it, total)));
    }

    // Generate conjunctive clauses till minimum activation is
    // reached. Note that the best precision (sao / active) can never
    // increase for each new mpv.  Despite this, we keep going until
    // at least min_activation is reached. It's not clear this
    // actually gives the best candidate one can get if min_activation
    // isn't reached, but we don't want to go below min activation
    // anyway, so it's an acceptable inacurracy.  (It would be a
    // problem only if activation constraint is very loose.)
    //
    unsigned active = 0;
    combo_tree tr;
    auto head = tr.set_head(id::logical_or);
    reverse_foreach (const auto& v, ptc) {
        active += v.second.second;

        // build the disjunctive clause
        auto dch = tr.append_child(head, id::logical_and);
        arity_t idx = 1;
        foreach(const auto& input, v.second.first->first) {
            argument arg(input == id::logical_true? idx++ : -idx++);
            tr.append_child(dch, arg);
        }

        // termination conditional
        if (ctable_usize * min_activation <= active)
            break;
    }
    return tr;
}

//////////////////////////////
// discretize_contin_bscore //
//////////////////////////////

// Note that this function returns a POSITIVE number, since p < 0.5
score_t discrete_complexity_coef(unsigned alphabet_size, double p)
{
    return -log((double)alphabet_size) / log(p/(1-p));
}

discretize_contin_bscore::discretize_contin_bscore(const OTable& ot,
                                                   const ITable& it,
                                                   const vector<contin_t>& thres,
                                                   bool wa)
    : target(ot), cit(it), thresholds(thres), weighted_accuracy(wa),
      classes(ot.size()), weights(thresholds.size() + 1, 1) {
    // enforce that thresholds is sorted
    boost::sort(thresholds);
    // precompute classes
    boost::transform(target, classes.begin(), [&](const vertex& v) {
            return this->class_idx(get_contin(v)); });
    // precompute weights
    multiset<size_t> cs(classes.begin(), classes.end());
    if (weighted_accuracy)
        for (size_t i = 0; i < weights.size(); ++i)
            weights[i] = classes.size() / (float)(weights.size() * cs.count(i));
}

behavioral_score discretize_contin_bscore::best_possible_bscore() const
{
    return behavioral_score(target.size(), 0);
}

score_t discretize_contin_bscore::min_improv() const
{
    // not necessarily right, just the backwards-compat behavior
    return 0.0;
}

size_t discretize_contin_bscore::class_idx(contin_t v) const
{
    if (v < thresholds[0])
        return 0;
    size_t s = thresholds.size();
    if (v >= thresholds[s - 1])
        return s;
    return class_idx_within(v, 1, s);
}

size_t discretize_contin_bscore::class_idx_within(contin_t v,
                                                  size_t l_idx,
                                                  size_t u_idx) const
{
    // base case
    if(u_idx - l_idx == 1)
        return l_idx;
    // recursive case
    size_t m_idx = l_idx + (u_idx - l_idx) / 2;
    contin_t t = thresholds[m_idx - 1];
    if(v < t)
        return class_idx_within(v, l_idx, m_idx);
    else
        return class_idx_within(v, m_idx, u_idx);
}

penalized_behavioral_score discretize_contin_bscore::operator()(const combo_tree& tr) const
{
    /// @todo could be optimized by avoiding computing the OTable and
    /// directly using the results on the fly. On really big table
    /// (dozens of thousands of data points and about 100 inputs, this
    /// has overhead of about 10% of the overall time)
    OTable ct(tr, cit);
    penalized_behavioral_score pbs(
        make_pair<behavioral_score, score_t>(behavioral_score(target.size()), 0));
    boost::transform(ct, classes, pbs.first.begin(), [&](const vertex& v, size_t c_idx) {
            return (c_idx != this->class_idx(get_contin(v))) * this->weights[c_idx];
        });

    // Add the Occam's razor feature
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    // Logger
    log_candidate_pbscore(tr, pbs);
    // ~Logger

    return pbs;    
}

/////////////////////////
// ctruth_table_bscore //
/////////////////////////
        
penalized_behavioral_score ctruth_table_bscore::operator()(const combo_tree& tr) const
{
    //penalized_behavioral_score pbs(
    //    make_pair<behavioral_score, score_t>(behavioral_score(target.size()), 0));
    penalized_behavioral_score pbs;

    // Evaluate the bscore components for all rows of the ctable
    foreach (const CTable::value_type& vct, ctable) {
        const vertex_seq& vs = vct.first;
        const CTable::counter_t& c = vct.second;
        pbs.first.push_back(-score_t(c.get(negate_vertex(eval_binding(vs, tr)))));
    }

    // Add the Occam's razor feature
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

behavioral_score ctruth_table_bscore::best_possible_bscore() const
{
    behavioral_score bs;
    transform(ctable | map_values, back_inserter(bs),
              [](const CTable::counter_t& c) {
                  // OK, this looks like magic, but here's what it does:
                  // CTable is a compressed table; multiple rows may
                  // have identical inputs, differing only in output.
                  // Clearly, in such a case, both outputs cannot be
                  // simultanously satisfied, but we can try to satisfy
                  // the one of which there is more.  Thus, we take
                  // the min of the two possiblities.
                  return -score_t(min(c.get(id::logical_true),
                                      c.get(id::logical_false)));
              });

    return bs;
}

score_t ctruth_table_bscore::min_improv() const
{
    return 0.5;
}


/////////////////////////
// enum_table_bscore //
/////////////////////////
        
penalized_behavioral_score enum_table_bscore::operator()(const combo_tree& tr) const
{
    penalized_behavioral_score pbs;

    // Evaluate the bscore components for all rows of the ctable
    foreach (const CTable::value_type& vct, ctable) {
        const vertex_seq& vs = vct.first;
        const CTable::counter_t& c = vct.second;
        // The number that are wrong equals total minus num correct.
        score_t sc = score_t(c.get(eval_binding(vs, tr)));
        sc -= score_t(c.total_count());
        pbs.first.push_back(sc);
    }

    // Add the Occam's razor feature
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

behavioral_score enum_table_bscore::best_possible_bscore() const
{
    behavioral_score bs;
    transform(ctable | map_values, back_inserter(bs),
              [](const CTable::counter_t& c) {
                  // OK, this looks like magic, but here's what it does:
                  // CTable is a compressed table; multiple rows may
                  // have identical inputs, differing only in output.
                  // Clearly, in such a case, different outputs cannot be
                  // simultanously satisfied, but we can try to satisfy
                  // the one of which there is the most.
                  unsigned most = 0;
                  CTable::counter_t::const_iterator it = c.begin();
                  for (; it != c.end(); it++) {
                      if (most < it->second) most = it->second;
                  }
                  return score_t (most - c.total_count());
              });

    return bs;
}

score_t enum_table_bscore::min_improv() const
{
    return 0.5;
}

/////////////////////////
// enum_filter_bscore //
/////////////////////////
        
penalized_behavioral_score enum_filter_bscore::operator()(const combo_tree& tr) const
{
    penalized_behavioral_score pbs;

    typedef combo_tree::sibling_iterator sib_it;
    typedef combo_tree::iterator pre_it;

    pre_it it = tr.begin();
    if (is_enum_type(*it)) 
        return enum_table_bscore::operator()(tr);

    OC_ASSERT(*it == id::cond, "Error: unexpcected candidate!");
    sib_it predicate = it.begin();
    vertex consequent = *next(predicate);

    // Evaluate the bscore components for all rows of the ctable
    foreach (const CTable::value_type& vct, ctable) {
        const vertex_seq& vs = vct.first;
        const CTable::counter_t& c = vct.second;

        unsigned total = c.total_count();

        // The number that are wrong equals total minus num correct.
        score_t sc = score_t(c.get(eval_binding(vs, tr)));
        sc -= score_t(total);

        // Punish the first predicate, if it is wrong.
        vertex pr = eval_throws_binding(vs, predicate);
        if (pr == id::logical_true) {
            if (total != c.get(consequent))
                sc -= punish * total;
        }

        pbs.first.push_back(sc);
    }

    // Add the Occam's razor feature
    if (occam)
        pbs.second = tree_complexity(tr) * complexity_coef;

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

/////////////////////////
// enum_graded_bscore //
/////////////////////////

/// OK, the goal here is to compute the "graded" tree complexity.
/// Much the same way as the score is graded below, we want to do
/// the same for the complexity, so that complex later predicates
/// don't (do?) dominate the the penalty.  Actually, this is
/// retro-graded: punish more complex, later predicates...
score_t enum_graded_bscore::graded_complexity(combo_tree::iterator it) const
{
    typedef combo_tree::sibling_iterator sib_it;
    typedef combo_tree::iterator pre_it;
    sib_it predicate = it.begin();
    score_t cpxy = 0.0;
    score_t weight = 1.0;
    while (1) {
        cpxy += weight * score_t(tree_complexity((pre_it) predicate));

        // Is it the last one, the else clause?
        if (is_enum_type(*predicate))
            break;

        // advance
        predicate = next(predicate, 2);
        weight /= grading;

    }
    return cpxy;
}
        
penalized_behavioral_score enum_graded_bscore::operator()(const combo_tree& tr) const
{
    penalized_behavioral_score pbs;

    typedef combo_tree::sibling_iterator sib_it;
    typedef combo_tree::iterator pre_it;

    pre_it it = tr.begin();
    if (is_enum_type(*it)) 
        return enum_table_bscore::operator()(tr);

    OC_ASSERT(*it == id::cond, "Error: unexpcected candidate!");

    // Evaluate the bscore components for all rows of the ctable
    foreach (const CTable::value_type& vct, ctable) {
        const vertex_seq& vs = vct.first;
        const CTable::counter_t& c = vct.second;

        unsigned total = c.total_count();
        score_t weight = 1.0;

        sib_it predicate = it.begin();
        // The number that are wrong equals total minus num correct.
        score_t sc = -score_t(total);
        while (1) {
            // Is it the last one, the else clause?
            if (is_enum_type(*predicate)) {
                vertex consequent = *predicate;
                sc += c.get(consequent);
                sc *= weight;
                break;
            }
    
            // The first true predicate terminates.
            vertex pr = eval_throws_binding(vs, predicate);
            if (pr == id::logical_true) {
                vertex consequent = *next(predicate);
                sc += c.get(consequent);
                sc *= weight;
                break;
            }

            // advance
            predicate = next(predicate, 2);
            weight *= grading;
        }
        pbs.first.push_back(sc);
    }

    // Add the Occam's razor feature
    pbs.second = 0.0;
    if (occam) {
        // pbs.second = tree_complexity(tr) * complexity_coef;
        pbs.second = graded_complexity(it) * complexity_coef;
    }

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

score_t enum_graded_bscore::min_improv() const
{
    // Negative values are interpreted as percentages by the optimizer.
    // So -0.05 means "a 5% improvement".  Problem is, the grading
    // wrecks any sense of an absolute score improvement...
    return -0.05;
}

// Much like enum_graded_score, above, except that we exchange the 
// inner and outer loops.  This makes the algo slower and bulkier, but
// it does allow the effectiveness of predicates to be tracked.
//
penalized_behavioral_score enum_effective_bscore::operator()(const combo_tree& tr) const
{
    penalized_behavioral_score pbs;

    typedef combo_tree::sibling_iterator sib_it;
    typedef combo_tree::iterator pre_it;

    pbs.first = behavioral_score(_ctable_usize);

    // Is this just a constant? Then just add them up.
    pre_it it = tr.begin();
    if (is_enum_type(*it)) {
        behavioral_score::iterator bit = pbs.first.begin();
        foreach (const CTable::value_type& vct, ctable) {
            const CTable::counter_t& c = vct.second;

            // The number that are wrong equals total minus num correct.
            *bit++ = c.get(*it) - score_t(c.total_count());
        }
        return pbs;
    }

    OC_ASSERT(*it == id::cond, "Error: unexpcected candidate!");

    // Accumulate the score with multiple passes, so zero them out here.
    foreach (score_t& sc, pbs.first) sc = 0.0;

    // Are we done yet?
    vector<bool> done(_ctable_usize);
    vector<bool>::iterator dit = done.begin();
    for (; dit != done.end(); dit++) *dit = false;

    sib_it predicate = it.begin();
    score_t weight = 1.0;
    while (1) {

        // Is it the last one, the else clause?
        if (is_enum_type(*predicate)) {
            vertex consequent = *predicate;

            behavioral_score::iterator bit = pbs.first.begin();
            vector<bool>::iterator dit = done.begin();
            foreach (const CTable::value_type& vct, ctable) {
                if (*dit == false) {
                    const CTable::counter_t& c = vct.second;

                    // The number that are wrong equals total minus num correct.
                    score_t sc = -score_t(c.total_count());
                    sc += c.get(consequent);
                    *bit += weight * sc;
                }
                bit++;
                dit++;
            }
            break;
        }

        vertex consequent = *next(predicate);

        // Evaluate the bscore components for all rows of the ctable
        behavioral_score::iterator bit = pbs.first.begin();
        vector<bool>::iterator dit = done.begin();

        bool effective = false;
        foreach (const CTable::value_type& vct, ctable) {
            if (*dit == false) {
                const vertex_seq& vs = vct.first;
                const CTable::counter_t& c = vct.second;

                vertex pr = eval_throws_binding(vs, predicate);
                if (pr == id::logical_true) {
                    int sc = c.get(consequent);
                    // A predicate is effective if it evaluates to true,
                    // and at least gets a right answr when it does...
                    if (0 != sc) effective = true;

                    // The number that are wrong equals total minus num correct.
                    sc -= c.total_count();
                    *bit += weight * score_t(sc);

                    *dit = true;
                }
            }
            bit++;
            dit++;
        }

        // advance
        predicate = next(predicate, 2);
        if (effective) weight *= grading;
    }

    // Add the Occam's razor feature
    pbs.second = 0.0;
    if (occam) {
        // pbs.second = tree_complexity(tr) * complexity_coef;
        pbs.second = graded_complexity(it) * complexity_coef;
    }

    log_candidate_pbscore(tr, pbs);

    return pbs;
}

//////////////////////////////////
// interesting_predicate_bscore //
//////////////////////////////////

interesting_predicate_bscore::interesting_predicate_bscore(const CTable& ctable_,
                                                           weight_t kld_w_,
                                                           weight_t skewness_w_,
                                                           weight_t stdU_w_,
                                                           weight_t skew_U_w_,
                                                           score_t min_activation_,
                                                           score_t max_activation_,
                                                           score_t penalty_,
                                                           bool positive_,
                                                           bool abs_skewness_,
                                                           bool decompose_kld_)
    : ctable(ctable_),
      kld_w(kld_w_), skewness_w(skewness_w_), abs_skewness(abs_skewness_),
      stdU_w(stdU_w_), skew_U_w(skew_U_w_), min_activation(min_activation_),
      max_activation(max_activation_), penalty(penalty_), positive(positive_),
      decompose_kld(decompose_kld_)
{
    // define counter (mapping between observation and its number of occurences)
    boost::for_each(ctable | map_values, [this](const CTable::mapped_type& mv) {
            boost::for_each(mv, [this](const CTable::mapped_type::value_type& v) {
                    counter[get_contin(v.first)] += v.second; }); });
    // precompute pdf
    if (kld_w > 0) {
        pdf = counter;
        klds.set_p_pdf(pdf);
    }
    // compute the skewness of pdf
    accumulator_t acc;
    foreach(const auto& v, pdf)
        acc(v.first, weight = v.second);
    skewness = weighted_skewness(acc);
    logger().fine("skewness = %f", skewness);
}

penalized_behavioral_score interesting_predicate_bscore::operator()(const combo_tree& tr) const
{
    OTable pred_ot(tr, ctable);

    vertex target = bool_to_vertex(positive);
    
    unsigned total = 0, // total number of observations (could be optimized)
        actives = 0; // total number of positive (or negative if
                     // positive is false) predicate values
    boost::for_each(ctable | map_values, pred_ot,
                    [&](const CTable::counter_t& c, const vertex& v) {
                        unsigned tc = c.total_count();
                        if (v == target)
                            actives += tc;
                        total += tc;
                    });

    logger().fine("total = %u", total);
    logger().fine("actives = %u", actives);

    penalized_behavioral_score pbs;
    behavioral_score &bs = pbs.first;

    // filter the output according to pred_ot
    counter_t pred_counter;     // map obvervation to occurence
                                // conditioned by predicate truth
    boost::for_each(ctable | map_values, pred_ot,
                    [&](const CTable::counter_t& c, const vertex& v) {
                        if (v == target) {
                            foreach(const auto& mv, c)
                                pred_counter[get_contin(mv.first)] = mv.second;
                        }});

    logger().fine("pred_ot.size() = %u", pred_ot.size());
    logger().fine("pred_counter.size() = %u", pred_counter.size());

    if (pred_counter.size() > 1) { // otherwise the statistics are
                                   // messed up (for instance
                                   // pred_skewness can be inf)
        // compute KLD
        if (kld_w > 0) {
            if (decompose_kld) {
                klds(pred_counter, back_inserter(bs));
                boost::transform(bs, bs.begin(), kld_w * arg1);
            } else {
                score_t pred_klds = klds(pred_counter);
                logger().fine("klds = %f", pred_klds);
                bs.push_back(kld_w * pred_klds);
            }
        }

        if (skewness_w > 0 || stdU_w > 0 || skew_U_w > 0) {
            
            // gather statistics with a boost accumulator
            accumulator_t acc;
            foreach(const auto& v, pred_counter)
                acc(v.first, weight = v.second);

            score_t diff_skewness = 0;
            if (skewness_w > 0 || skew_U_w > 0) {
                // push the absolute difference between the
                // unconditioned skewness and conditioned one
                score_t pred_skewness = weighted_skewness(acc);
                diff_skewness = pred_skewness - skewness;
                score_t val_skewness = (abs_skewness?
                                        abs(diff_skewness):
                                        diff_skewness);
                logger().fine("pred_skewness = %f", pred_skewness);
                if (skewness_w > 0)
                    bs.push_back(skewness_w * val_skewness);
            }

            score_t stdU = 0;
            if (stdU_w > 0 || skew_U_w > 0) {

                // compute the standardized Mann–Whitney U
                stdU = standardizedMannWhitneyU(counter, pred_counter);
                logger().fine("stdU = %f", stdU);
                if (stdU_w > 0)
                    bs.push_back(stdU_w * abs(stdU));
            }
                
            // push the product of the relative differences of the
            // shift (stdU) and the skewness (so that if both go
            // in the same direction the value if positive, and
            // negative otherwise)
            if (skew_U_w > 0)
                bs.push_back(skew_U_w * stdU * diff_skewness);
        }
            
        // add activation_penalty component
        score_t activation = actives / (score_t) total;
        score_t activation_penalty = get_activation_penalty(activation);
        logger().fine("activation = %f", activation);
        logger().fine("activation penalty = %e", activation_penalty);
        bs.push_back(activation_penalty);
        
        // add the Occam's razor feature
        if (occam)
            pbs.second = tree_complexity(tr) * complexity_coef;
    } else {
        pbs.first.push_back(worst_score);
    }

    // Logger
    log_candidate_pbscore(tr, pbs);
    // ~Logger

    return pbs;
}

behavioral_score interesting_predicate_bscore::best_possible_bscore() const
{
    return behavioral_score(1, best_score);
}

void interesting_predicate_bscore::set_complexity_coef(unsigned alphabet_size,
                                                       float stdev)
{
    complexity_coef = 0.0;
    occam = stdev > 0;
    if (occam)
        complexity_coef = contin_complexity_coef(alphabet_size, stdev);

    logger().info() << "intersting_predicate_bscore noise = " << stdev
                    << " alphabest size = " << alphabet_size
                    << " complexity ratio = " << 1.0/complexity_coef;
}

score_t interesting_predicate_bscore::get_activation_penalty(score_t activation) const
{
    score_t dst = max(max(min_activation - activation, score_t(0))
                      / min_activation,
                      max(activation - max_activation, score_t(0))
                      / (1 - max_activation));
    logger().fine("dst = %f", dst);
    return log(pow((1 - dst), penalty));
}

score_t interesting_predicate_bscore::min_improv() const
{
    return 0.0;                 // not necessarily right, just the
                                // backward behavior
}

} // ~namespace moses
} // ~namespace opencog
