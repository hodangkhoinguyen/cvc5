/******************************************************************************
 * Top contributors (to current version):
 *   Kartik Sabharwal, Andrew Reynolds, Mathias Preiner
 *
 * This file is part of the cvc5 project.
 *
 * Copyright (c) 2009-2025 by the authors listed in the file AUTHORS
 * in the top-level source directory and their institutional affiliations.
 * All rights reserved.  See the file COPYING in the top-level source
 * directory for licensing information.
 * ****************************************************************************
 *
 * conjecture generator class
 */

#include "cvc5_private.h"

#ifndef CONJECTURE_GENERATOR_H
#define CONJECTURE_GENERATOR_H

#include "context/cdhashmap.h"
#include "expr/node_trie.h"
#include "expr/term_canonize.h"
#include "smt/env_obj.h"
#include "theory/quantifiers/quant_module.h"
#include "theory/type_enumerator.h"

namespace cvc5::internal {
namespace theory {
namespace quantifiers {

//algorithm for computing candidate subgoals

class ConjectureGenerator;

/** operator independent index of arguments for an EQC
 *
 * The (almost) inductive definition of the set of irrelevant terms suggests
 * the following algorithm to compute I, the set of irrelevant equivalence
 * classes.  It is a standard algorithm that starts from the empty set and
 * iterates up to a fixed point.
 *
 *     declare I and set its value to the empty set
 *
 *     for each equivalence class e:
 *       if the sort of e is not an inductive datatype sort:
 *         add e to I
 *
 *     declare I_old
 *
 *     do:
 *        set I_old to the current value of I
 *        for each equivalence class e:
 *          if e is not in I:
 *            for each term t in e:
 *              if t has the form f(t_1, ..., t_n) where f is an atomic
 *              trigger that is not a skolem function and the equivalence class
 *              of each t_i is in I:
 *                add e to I
 *                continue to the next equivalence class
 *              else
 *                continue to the next term in e
 *     while I_old is different from I
 *
 * Note the three nested loops in the second phase of the algorithm above.
 * We can avoid inspecting each element of each equivalence class that is not
 * already in I by preparing a family of indices, which can be described
 * abstractly as follows.
 *
 * _Definitions_
 *
 * - 'E' is the set of representatives of equivalence classes.
 * - 'F' is the set of function symbols.
 * - 'T' is the set of terms.
 * - For a set 'X', X* denotes the set of all strings over X.
 * - For a set 'X', X+ denotes the set of non-empty strings over X.
 * - For sets 'X' and 'Y', X x Y denotes their cartesian product.
 * - 't = u' means the terms t and u are syntactically equal.
 * - 't ~ u' means the terms t and u are in the same equivalence class according
 * to the current model
 *
 * _Declarations_
 *
 * - 'OpArgIndex' is a subset of E+, we intend that OpArgIndex(e e_1 ... e_n) is
 * true iff the string e e_1 ... e_n denotes an instance of the C++ class named
 * OpArgIndex
 *
 * - '[[e e_1 ... e_n]]' is the instance of the OpArgIndex class denoted by the
 * string e e_1 ... e_n when OpArgIndex(e e_1 ... e_n) is true, and it is
 * equal to d_op_arg_index[e].d_child[e_1].(...).d_child[e_n]
 *
 * - 'child' is a subset of E+ x E, we intend that child(e e_1 ... e_n, e^) is
 * true iff the map [[e e_1 ... e_n]].d_child contains e^ as a key.
 *
 * - 'ops' : OpArgIndex -> F* is a function where we intend ops(e e_1 ... e_n)
 * to be the same sequence of function symbols as the vector
 * [[e e_1 ... e_n]].d_ops
 *
 * - 'op_terms' : OpArgIndex -> T* is a function where we intend
 * op_terms(e e_1 ... e_n) to be the same sequence of terms as the vector
 * [[e e_1 ... e_n]].d_op_terms
 *
 * - 'added' is a subset of E x T where we intend added(e, t) to be true iff
 * d_op_arg_index[e].addTerm(t) executes successfully.
 *
 * _Invariants_
 *
 * (i)  child(e e_1 ... e_n, e^)
 * <==> OpArgIndex(e e_1 ... e_n e^)
 *
 * (ii) OpArgIndex(e e_1 ... e_n)
 *  ==> for all 0 <= i < n. OpArgIndex(e e_1 ... e_i)
 *
 * (iii) added(e, f(t_1, ..., t_n))
 *  <==> OpArgIndex(e e_1 ... e_n)
 *    /\ there exists j. ops(e e_1 ... e_n)(j) = f
 *                    /\ op_terms(e e_1 ... e_n)(j) = f(t_1, ..., t_n)
 *
 * (iv) d_ops(e e_1 ... e_n) has the same length as |d_op_terms(e e_1 ... e_n)|
 *
 * _Additional guarantees_
 *
 * In the implementation of getEquivalenceClasses, note that we add
 * f(t_1, ..., t_n) to d_op_terms[e] when, among satisfying certain other
 * properties, it is in e's equivalence class. This guarantees that
 * added(e, f(t_1, ..., t_n)) ==> f(t_1, ..., t_n) ~ e.
 *
 * Furthermore the implementation of addTerm ensures that for any equivalence
 * class representative e and for any two terms t = f(t_1, ..., t_n) and
 * u = f(u_1, ..., u_n) such that t != u, t ~ e, u ~ e,
 * and t_i ~ u_i for each i, we that at most one of added(e, t) and
 * added(e, u) is true.
 *
 * _Take-away_
 *
 * The problem of deciding whether the equivalence class represented by e is
 * irrelevant (see comment for computeIrrelevantEqcs) falls to searching for
 * a string e e_1 ... e_n such that
 *
 * - OpArgIndex(e e_1 ... e_n), and
 * - for all 1 <= i < n. e_i is irrelevant, and
 * - ops(e e_1 ... e_n) is non-empty.
 *
 * as implemented in 'getGroundTerms'.
 *
 * We hope is that searching for such a string is more efficient than the
 * naive approach of iterating over all terms in e's equivalence class and
 * checking if any one of these terms is irrelevant.
 *
 * _Example_
 *
 * Let e, e_1, e_2 and e_3 be representatives of equivalence classes.
 * Suppose we're given that
 *
 * - f(t_1,t_2) ~ g(t_3) ~ f(t_4,t_5) ~ f(t_6,t_7) ~ e, and
 * - t_1 ~ t_3 ~ t_4 ~ t_6 ~ e_1, and
 * - t_2 ~ t_5 ~ e_2, and
 * - t_7 ~ e_3
 *
 * Suppose also that we add f(t_1,t_2), g(t_3), f(t_4,t_5), and f(t_6,t_7)
 * to d_op_arg_index[e] in sequence.  The resulting data structure looks like
 *
 *   [[e]], d_ops = [], d_op_terms = []
 *     |
 * e_1 '-- [[e e_1]], d_ops = [ g ], d_op_terms = [ g(t_3) ]
 *            |
 *        e_2 |-- [[e e_1 e_2]], d_ops = [ f ], d_op_terms = [ f(t_4,t_5) ]
 *            |
 *        e_3 '-- [[e e_1 e_3]], d_ops = [ f ], d_op_terms = [ f(t_6,t_7) ]
 */
class OpArgIndex
{
public:
  std::map< TNode, OpArgIndex > d_child;
  std::vector< TNode > d_ops;
  std::vector< TNode > d_op_terms;
  void addTerm( std::vector< TNode >& terms, TNode n, unsigned index = 0 );
  Node getGroundTerm( ConjectureGenerator * s, std::vector< TNode >& args );
  void getGroundTerms( ConjectureGenerator * s, std::vector< TNode >& terms );
};

class PatternTypIndex
{
public:
  std::vector< TNode > d_terms;
  std::map< TypeNode, std::map< unsigned, PatternTypIndex > > d_children;
  void clear() {
    d_terms.clear();
    d_children.clear();
  }
};

class SubstitutionIndex
{
public:
  //current variable, or ground EQC if d_children.empty()
  TNode d_var;
  std::map< TNode, SubstitutionIndex > d_children;
  //add substitution
  void addSubstitution( TNode eqc, std::vector< TNode >& vars, std::vector< TNode >& terms, unsigned i = 0 );
  //notify substitutions
  bool notifySubstitutions( ConjectureGenerator * s, std::map< TNode, TNode >& subs, TNode rhs, unsigned numVars, unsigned i = 0 );
};

class TermGenEnv;

class TermGenerator
{
 private:
  unsigned calculateGeneralizationDepth( TermGenEnv * s, std::map< TypeNode, std::vector< int > >& fvs );

 public:
  TermGenerator()
      : d_id(0),
        d_status(0),
        d_status_num(0),
        d_status_child_num(0),
        d_match_status(0),
        d_match_status_child_num(0),
        d_match_mode(0)
  {
  }
  TypeNode d_typ;
  unsigned d_id;
  //1 : consider as unique variable
  //2 : consider equal to another variable
  //5 : consider a function application
  unsigned d_status;
  int d_status_num;
  //for function applications: the number of children you have built
  int d_status_child_num;
  //children (pointers to TermGenerators)
  std::vector< unsigned > d_children;

  //match status
  int d_match_status;
  int d_match_status_child_num;
  //match mode bits
  //0 : different variables must have different matches
  //1 : variables must map to ground terms
  //2 : variables must map to non-ground terms
  unsigned d_match_mode;
  //children
  std::vector<std::map<TNode, TNodeTrie>::iterator> d_match_children;
  std::vector<std::map<TNode, TNodeTrie>::iterator> d_match_children_end;

  void reset( TermGenEnv * s, TypeNode tn );
  bool getNextTerm( TermGenEnv * s, unsigned depth );
  void resetMatching( TermGenEnv * s, TNode eqc, unsigned mode );
  bool getNextMatch( TermGenEnv * s, TNode eqc, std::map< TypeNode, std::map< unsigned, TNode > >& subs, std::map< TNode, bool >& rev_subs );

  unsigned getDepth( TermGenEnv * s );
  unsigned getGeneralizationDepth( TermGenEnv * s );
  Node getTerm( TermGenEnv * s );

  void debugPrint( TermGenEnv * s, const char * c, const char * cd );
};


class TermGenEnv
{
public:
  //collect signature information
  void collectSignatureInformation();
  //reset function
  void reset( unsigned gdepth, bool genRelevant, TypeNode tgen );
  //get next term
  bool getNextTerm();
  //reset matching
  void resetMatching( TNode eqc, unsigned mode );
  //get next match
  bool getNextMatch( TNode eqc, std::map< TypeNode, std::map< unsigned, TNode > >& subs, std::map< TNode, bool >& rev_subs );
  //get term
  Node getTerm();
  //debug print
  void debugPrint( const char * c, const char * cd );

  //conjecture generation
  ConjectureGenerator * d_cg;
  //the current number of enumerated variables per type
  std::map< TypeNode, unsigned > d_var_id;
  //the limit of number of variables per type to enumerate
  std::map< TypeNode, unsigned > d_var_limit;
  //the functions we can currently generate
  std::map< TypeNode, std::vector< TNode > > d_typ_tg_funcs;
  // whether functions must add operators
  std::map< TNode, bool > d_tg_func_param;
  //the equivalence classes (if applicable) that match the currently generated term
  bool d_gen_relevant_terms;
  //relevant equivalence classes
  std::vector< TNode > d_relevant_eqc[2];
  //candidate equivalence classes
  std::vector< std::vector< TNode > > d_ccand_eqc[2];
  //the term generation objects
  unsigned d_tg_id;
  std::map< unsigned, TermGenerator > d_tg_alloc;
  unsigned d_tg_gdepth;
  int d_tg_gdepth_limit;

  //all functions
  std::vector< TNode > d_funcs;
  //function to kind map
  std::map< TNode, Kind > d_func_kind;
  //type of each argument of the function
  std::map< TNode, std::vector< TypeNode > > d_func_args;

  //access functions
  unsigned getNumTgVars( TypeNode tn );
  bool allowVar( TypeNode tn );
  void addVar( TypeNode tn );
  void removeVar( TypeNode tn );
  unsigned getNumTgFuncs( TypeNode tn );
  TNode getTgFunc( TypeNode tn, unsigned i );
  Node getFreeVar( TypeNode tn, unsigned i );
  bool considerCurrentTerm();
  bool considerCurrentTermCanon( unsigned tg_id );
  void changeContext( bool add );
  bool isRelevantFunc( Node f );
  bool isRelevantTerm( Node t );
  //carry
  TermDb * getTermDatabase();
  Node getGroundEqc( TNode r );
  bool isGroundEqc( TNode r );
  bool isGroundTerm( TNode n );
};



class TheoremIndex
{
private:
  void addTheorem( std::vector< TNode >& lhs_v, std::vector< unsigned >& lhs_arg, TNode rhs );
  void addTheoremNode( TNode curr, std::vector< TNode >& lhs_v, std::vector< unsigned >& lhs_arg, TNode rhs );
  void getEquivalentTerms( std::vector< TNode >& n_v, std::vector< unsigned >& n_arg,
                           std::map< TNode, TNode >& smap, std::vector< TNode >& vars, std::vector< TNode >& subs,
                           std::vector< Node >& terms );
  void getEquivalentTermsNode( Node curr, std::vector< TNode >& n_v, std::vector< unsigned >& n_arg,
                               std::map< TNode, TNode >& smap, std::vector< TNode >& vars, std::vector< TNode >& subs,
                               std::vector< Node >& terms );
public:
  std::map< TypeNode, TNode > d_var;
  std::map< TNode, TheoremIndex > d_children;
  std::vector< Node > d_terms;

  void addTheorem( TNode lhs, TNode rhs ) {
    std::vector< TNode > v;
    std::vector< unsigned > a;
    addTheoremNode( lhs, v, a, rhs );
  }
  void getEquivalentTerms( TNode n, std::vector< Node >& terms ) {
    std::vector< TNode > nv;
    std::vector< unsigned > na;
    std::map< TNode, TNode > smap;
    std::vector< TNode > vars;
    std::vector< TNode > subs;
    getEquivalentTermsNode( n, nv, na, smap, vars, subs, terms );
  }
  void clear(){
    d_var.clear();
    d_children.clear();
    d_terms.clear();
  }
  void debugPrint( const char * c, unsigned ind = 0 );
};



class ConjectureGenerator : public QuantifiersModule
{
  friend class OpArgIndex;
  friend class PatGen;
  friend class PatternGenEqc;
  friend class PatternGen;
  friend class SubsEqcIndex;
  friend class TermGenerator;
  friend class TermGenEnv;
  typedef context::CDHashMap<Node, Node> NodeMap;
  typedef context::CDHashMap<Node, bool> BoolMap;
  // this class maintains a congruence closure for *universal* facts
 private:
  //notification class for equality engine
  class NotifyClass : public eq::EqualityEngineNotify {
    ConjectureGenerator& d_sg;
  public:
    NotifyClass(ConjectureGenerator& sg): d_sg(sg) {}
    bool eqNotifyTriggerPredicate(TNode predicate, bool value) override
    {
      return true;
    }
    bool eqNotifyTriggerTermEquality(TheoryId tag,
                                     TNode t1,
                                     TNode t2,
                                     bool value) override
    {
      return true;
    }
    void eqNotifyConstantTermMerge(TNode t1, TNode t2) override {}
    void eqNotifyNewClass(TNode t) override { d_sg.eqNotifyNewClass(t); }
    void eqNotifyMerge(TNode t1, TNode t2) override
    {
      d_sg.eqNotifyMerge(t1, t2);
    }
    void eqNotifyDisequal(TNode t1, TNode t2, TNode reason) override
    {
    }
  };/* class ConjectureGenerator::NotifyClass */
  /** The notify class */
  NotifyClass d_notify;
  class EqcInfo{
  public:
   EqcInfo(context::Context* c);
   // representative
   context::CDO<Node> d_rep;
  };
  /** get or make eqc info */
  EqcInfo* getOrMakeEqcInfo( TNode n, bool doMake = false );
  /** boolean terms */
  Node d_true;
  Node d_false;
  /** (universal) equaltity engine */
  eq::EqualityEngine d_uequalityEngine;
  /** pending adds */
  std::vector< Node > d_upendingAdds;
  /** relevant terms */
  std::map< Node, bool > d_urelevant_terms;
  /** information necessary for equivalence classes */
  std::map< Node, EqcInfo* > d_eqc_info;
  /** called when a new equivalance class is created */
  void eqNotifyNewClass(TNode t);
  /** called when two equivalance classes have merged */
  void eqNotifyMerge(TNode t1, TNode t2);
  /** are universal equal */
  bool areUniversalEqual( TNode n1, TNode n2 );
  /** are universal disequal */
  bool areUniversalDisequal( TNode n1, TNode n2 );
  /** get universal representative */
  Node getUniversalRepresentative(TNode n, bool add = false);
  /** set relevant */
  void setUniversalRelevant( TNode n );
  /** ordering for universal terms */
  bool isUniversalLessThan( TNode rt1, TNode rt2 );

  /** the nodes we have reported as canonical representative */
  std::vector< TNode > d_ue_canon;
  /** is reported canon */
  bool isReportedCanon( TNode n );
  /** mark that term has been reported as canonical rep */
  void markReportedCanon( TNode n );

private:  //information regarding the conjectures
  /** list of all conjectures */
  std::vector< Node > d_conjectures;
  /** list of all waiting conjectures */
  std::vector< Node > d_waiting_conjectures_lhs;
  std::vector< Node > d_waiting_conjectures_rhs;
  std::vector< int > d_waiting_conjectures_score;
  /** map of currently considered equality conjectures */
  std::map< Node, std::vector< Node > > d_waiting_conjectures;
  /** map of equality conjectures */
  std::map< Node, std::vector< Node > > d_eq_conjectures;
  /** currently existing conjectures in equality engine */
  BoolMap d_ee_conjectures;
  /** conjecture index */
  TheoremIndex d_thm_index;
private:  //free variable list
  // get canonical free variable #i of type tn
  Node getFreeVar( TypeNode tn, unsigned i );
private:  //information regarding the terms
  //relevant patterns (the LHS's)
  std::map< TypeNode, std::vector< Node > > d_rel_patterns;
  //total number of unique variables
  std::map< TNode, unsigned > d_rel_pattern_var_sum;
  //by types
  PatternTypIndex d_rel_pattern_typ_index;
  // substitution to ground EQC index
  std::map< TNode, SubstitutionIndex > d_rel_pattern_subs_index;
  //patterns (the RHS's)
  std::map< TypeNode, std::vector< Node > > d_patterns;
  //patterns to # variables per type
  std::map< TNode, std::map< TypeNode, unsigned > > d_pattern_var_id;
  // # duplicated variables
  std::map< TNode, unsigned > d_pattern_var_duplicate;
  // is normal pattern?  (variables allocated in canonical way left to right)
  std::map< TNode, int > d_pattern_is_normal;
  std::map< TNode, int > d_pattern_is_relevant;
  // patterns to a count of # operators (variables and functions)
  std::map< TNode, std::map< TNode, unsigned > > d_pattern_fun_id;
  // term size
  std::map< TNode, unsigned > d_pattern_fun_sum;
  // collect functions
  unsigned collectFunctions( TNode opat, TNode pat, std::map< TNode, unsigned >& funcs,
                             std::map< TypeNode, unsigned >& mnvn, std::map< TypeNode, unsigned >& mxvn );
  // add pattern
  void registerPattern( Node pat, TypeNode tpat );
private: //for debugging
  std::map< TNode, unsigned > d_em;
  unsigned d_conj_count;
public:
  //term generation environment
  TermGenEnv d_tge;
  //consider term canon
  bool considerTermCanon( Node ln, bool genRelevant );
  /** collect equivalence classes
   *
   * This function iterates over the representative 'r'
   * of each equivalence class and
   *
   * - adds 'r' to 'eqcs',
   * - assigns to 'r' a 1-indexed serial number 'd_em[r]',
   * - and adds every term 't' in the equivalence class represented by 'r'
   * to the operator-argument index, which will be used to identify
   * equivalence classes that do not contain concrete terms and so are
   * relevant for conjecture generation.
   */
  void getEquivalenceClasses(std::vector<TNode>& eqcs);
  /** compute irrelevant equivalence classes
   *
   * This function populates 'd_ground_eqc_map' with irrelevant equivalence
   * classes.  In other words, it populates this map with key-value pairs
   * where each key is an irrelevant term that is also the representative of
   * some equivalence class.  The values are not important.
   *
   * In principle a term is *irrelevant* if and only if
   *
   *   1. it is not of inductive datatype sort,
   *   2. OR it is of inductive datatype sort,
   *     - AND it has an operator which is an *atomic trigger* but not a skolem
   *     function,
   *     - AND all of its immediate children -- the operator's arguments -- are
   *     themselves irrelevant terms,
   *   3. OR it is equivalent to an irrelevant term in the current model.
   *
   * The objective behind defining irrelevance this way is to narrow the set
   * of *relevant* terms to datatype-sorted terms that are impossible to express
   * using only non-datatype terms, constructors, selectors,
   * uninterpreted functions, or other function-like symbols (atomic triggers).
   *
   * Consequently the set of relevant terms contains (among other terms)
   * datatype-sorted skolem constants that represent "arbitrary" values of that
   * sort rather than "concrete" values.  These are precisely the skolem
   * constants we may want to translate into universally quantified variables
   * when synthesizing conjectures.
   */
  void computeIrrelevantEqcs(const std::vector<TNode>& eqcs);
  /** print information related to the computation of irrelevant equivalence
   * classes
   *
   * This function requires that the contents of the input vector 'eqcs' are
   * exactly the representatives of each equivalence class in the current model.
   *
   * For each element e of eqcs other than nodeManager()->mkConst(false), this
   * function prints all terms t ~ e such that t is active (according to the
   * term database) and t is not an equality.  If e is computed irrelevant (see
   * computeIrrelevantEqcs) this function also prints a term that explains why e
   * is irrelevant.
   */
  void debugPrintIrrelevantEqcs(const std::vector<TNode>& eqcs);
  /** compute relevant equivalence classes
   *
   * This function requires that the contents of the input vector eqcs are
   * exactly the representatives of each equivalence class in the current model.
   *
   * This function guarantees that
   *
   * - the contents of d_tge.d_relevant_eqc[0] are exactly the relevant
   * equivalence class representatives in eqcs,
   * - the contents of d_tge.d_relevant_eqc[1] are exactly the irrelevant
   * equivalence class representatives in eqcs, and
   * - eqcs is unchanged.
   */
  void computeRelevantEqcs(const std::vector<TNode>& eqcs);
  /** build theorem index from universally quantified formulas
   *
   * We look at all asserted formulas q of the below form and try to add their
   * data to both the theorem index (d_thm_index) and the universal equality
   * engine (d_uequalityEngine).
   *
   *     q := (forall x_1,...,x_n. l = r)
   *
   * However the theorem index expects equalities (t = u) that are in canonical
   * form, contain only *relevant* function symbols, and are not *subsumed*
   * (entailed in the universal equality engine). Therefore if q's body (l = r)
   * contains an irrelevant function symbol, we skip over q.  Otherwise we
   * canonize (l = r) to (l' = r') and check if it is subsumed.  If it is, we
   * skip over q.  At this point we know that (l' = r') is relevant, canonical
   * and not subsumed, so we (1) ensure that the equivalence classes of l' and
   * r' are merged in the universal equality engine, and (2) add both (l' = r')
   * and the canonical form of (r = l) to the theorem index.
   *
   * We also return all *proven conjectures* in a vector.  These are conjectures
   * that had been proposed in prior rounds (elements of d_conjectures) and are
   * asserted true in the current model.
   *
   * *Note.* There appear to be some unstated assumptions in the code.
   * - if q is in d_conjectures it is assumed that its body (l = r) contains
   * only relevant function symbols and is already in canonical form, and
   * - if (l = r) is in the equality engine it is assumed that its canonical
   * form (l' = r') is not subsumed and also that the equivalence classes of l'
   * and r' do not have to be merged in the universal equality engine.  (I'm not
   * sure that this is right.)
   */
  std::vector<Node> buildTheoremIndex();
  /* examine status of conjectures
   *
   * This is a debug printing function that does not modify the state of the
   * conjecture generator.  We iterate over all unproven conjectures q.  If all
   * of q's bound variables are irrelevant, we claim that q has been *disproven*
   * and print the values of these bound variables as a counterexample to q.  If
   * at least one of q's bound variables is still relevant we print all of q's
   * relevant bound variables and claim that q remains *active*.
   *
   * *Note.* From the documentation for buildTheoremIndex(), any element of
   * d_conjectures that is not in provenConj is considered an unproven
   * conjecture.
   */
  void debugPrintUnprovenConjectures(const std::vector<Node>& provenConj);
  /* print theorem index
   *
   * This function just calls d_thm_index.debugPrint().
   */
  void debugPrintTheoremIndex();
  /* print pattern-type index
   */
  void debugPrintPatternTypIndex();
  /* generate conjectures
   *
   * This function synthesizes candidate equality conjectures, removes "bad"
   * candidates, and submits the remainder to the inference manager.  For each
   * value d of generalization depth from 1 up to maxDepth (determined by a
   * command line option with default value 3), we synthesize a canonical[1]
   * left-hand term of (generalization) depth exactly d subject to a filter[2].
   * We then use the synthesized term as a pattern and employ e-matching to
   * compute a number of substitutions from the free variables in the pattern to
   * irrelevant terms.  We repeat the process till all possible left-hand terms
   * with depth d are exhausted.  Next, for each value d' of generalization
   * depth between 1 and d (inclusive) we synthesize a canonical right-hand term
   * of depth d' using the same free variables as the left-hand terms.  The
   * right-hand term is also run through a filter[2].  If d' < d, we process
   * conjectures l = r where l is any synthesized left-hand term of depth
   * exactly d and r is the just-synthesized right-hand term.  On the other hand
   * if d' = d, we store r for later.  We repeat till all possible right-hand
   * terms of depth d' are exhausted.  We then process conjectures l = r where l
   * is any synthesized term of depth up to and including d and r is any term of
   * depth exactly d.  Each processed conjecture is put through more checks.  It
   * is rejected if
   * - it has already been sent to the inference manager[3], or
   * - it is already waiting to be sent to the inference manager[3], or
   * - it is falsified by one of the aforementioned substitutions that maps free
   * variables to irrelevant terms[3], or
   * - it is non-canonical[4], or
   * - its "score" is below a threshold[4].
   *
   * [1]: See ConjectureGenerator::considerTermCanon()
   * [2]: See TermGenEnv::considerCurrentTerm()
   * [3]: See ConjectureGenerator::considerCandidateConjecture()
   * [4]: See ConjectureGenerator::flushWaitingConjectures()
   *
   * ---------------------------------------------------------------------------
   *
   * This function uses a number of variables.  Here's a short description for
   * each.
   *
   * rel_term_count.  For a particular value of *depth*, the "relevant term
   * count" is the number of canonical left-hand terms generated at that depth.
   *
   * rt_var_max.  "Right variable maximum" is a map from types to natural
   * numbers.  Given a type t, rt_var_max[t] is the largest index among all
   * canonical variables of type t that appear free in any left-hand term
   * synthesized so far.  For example, if rt_var_max[t] is 3 then the only free
   * variables of type t in the left-hand terms synthesized so far must be {t0,
   * t1, t2, t3}.  When we generate right-hand terms of less or same depth, we
   * run the lines
   *
   *     d_tge.d_var_id[t] = rt_var_max[t];
   *     d_tge.d_var_limit[t] = rt_var_max[t];
   *
   * for each type t to ensure that the right-hand terms draw from the same pool
   * of free variables.
   *
   * rt_types.  "Right types" stores the types of all the left-hand terms
   * synthesizesd so far by the term generator.  Since we want to build equality
   * conjectures, we only synthesize right-hand of types from rt_types.
   *
   * nn.  The most recently synthesized left-hand term.
   *
   * tnn.  The type of nn.
   *
   * conj_lhs.  conj_lhs[t][n] is the list of left-hand terms synthesized so far
   * with type t and generalization depth n.
   *
   * addedLemmas.  The number of lemmas sent to the instantiation manager.
   *
   * maxDepth.  Gets it value from options().quantifiers.conjectureGenMaxDepth.
   * We do not synthesize left-hand terms with generalization depth maxDepth.
   *
   * gsubs_vars.  Our description of generateConjectures() mentions that for
   * each synthesized left-hand term we compute substitutions from the free
   * variables in the term to irrelevant terms.  gsubs_vars lists the free
   * variables in the most recently synthesized left-hand term and is the domain
   * of such a substitution. Suppose d_tge.d_var_id has the form [(t_0, n_0),
   * (t_1, n_1), ..., (t_k, n_k)].  Then the first n_0 elements of gsubs_vars
   * are the first n_0 canonical variables of type t_0, the next n_1 elements of
   * gsubs_vars are the first n_1 canonical variables of type t_1, and more
   * generally the n_i elements of gsubs_vars from indices n_0 + ... + n_(i-1)
   * to n_0 + ... + n_i - 1 are the first n_i canonical variables of type t_i,
   * assuming 0 <= i <= k.
   *
   * d_rel_pattern_var_sum.  For any synthesized left-hand term t,
   * d_rel_pattern_var_sum[t] equals the length of t's gsubs_vars.
   *
   * typ_to_subs_index.  Let d_tge.d_var_id have the form [(t_1, n_1), ...,
   * (t_k, n_k)] where all t_i's are distinct and all n_i's are non-negative.
   * Let lhs denote the most recently generated canonical term.
   * d_rel_pattern_var_sum[lhs] is set to n_1 + ... + n_k.
   * typ_to_subs_index[t_i] is set to n_1 + ... + n_(i-1) for 1 <= i <= k.  For
   * i < k, the elements of gsubs_vars from index typ_to_subs_index[t_i] up to
   * but not including typ_to_subs_index[t_(i+1)] are the first n_i canonical
   * variables of type t_i.  The elements of gsubs_vars from index
   * typ_to_subs_index[t_k] up to but not including d_rel_pattern_var_sum[lhs]
   * are the first n_k canonical variables of type t_k.
   *
   * gsubs_terms.  Where gsubs_vars is the domain of a substitution from free
   * variables to irrelevant terms, gsubs_terms is the substitution's range.
   * For each i from 0 up to but not including d_rel_pattern_var_sum[nn], the
   * variable gsubs_vars[i] maps to the irrelevant term gsubs_terms[i].
   *
   * d_rel_pattern_typ_index.  This index stores synthesized left-hand terms
   * based on the type and count of its free variables.  All terms with exactly
   * 1 free variable of type t_1 and 2 free variables of type t_2 are stored in
   * the same node in this index.  Since all the terms we add to this index are
   * canonical, we are guaranteed that if a term uses n variables of type t_i
   * then these variables are exactly the first n canonical variables of t_i.
   * An example is:
   *
   *     null, 0 -> []
   *       Nat, 0 -> []
   *         Lst, 1 -> [(last L0), (append L0 nil)]
   *         Lst, 2 -> [(append L0 L1),
   *                    (cons (last L0) L1),
   *                    (last (append L0 L1))]
   *         Lst, 3 -> [(append L0 (append L1 L2)),
   *                    (append (append L0 L1) L2)]
   *       Nat, 1 -> []
   *         Lst, 0 -> [(cons N0 nil)]
   *         Lst, 1 -> [(cons N0 L0), (last (cons N0 L0))]
   *         Lst, 2 -> [(cons N0 (append L0 L1)), (append (cons N0 L0) L1)]
   *
   * *Note.*  d_rel_pattern_typ_index is never actually used.
   *
   * conj_rhs.  Suppose we have just synthesized left-hand terms of
   * generalization depth d and are synthesizing right-hand terms of depth d' <=
   * d.  Synthesized right-hand terms of type t and depth exactly d (i.e. when
   * d' = d) are stored in conj_rhs[t].
   */
  void generateConjectures();
public:  //for generalization
  //generalizations
  bool isGeneralization( TNode patg, TNode pat ) {
    std::map< TNode, TNode > subs;
    return isGeneralization( patg, pat, subs );
  }
  bool isGeneralization( TNode patg, TNode pat, std::map< TNode, TNode >& subs );
  // get generalization depth
  int calculateGeneralizationDepth( TNode n, std::vector< TNode >& fv );
private:
  //predicate for type
  std::map< TypeNode, Node > d_typ_pred;
  //get predicate for type
  Node getPredicateForType( TypeNode tn );
  /** get enumerate uf term
   *
   * This function adds up to #num f-applications to terms, where f is
   * n.getOperator(). These applications are enumerated in a fair manner based
   * on an iterative deepening of the sum of indices of the arguments. For
   * example, if f : U x U -> U, an the term enumerator for U gives t1, t2, t3
   * ..., then we add f-applications to terms in this order:
   *   f( t1, t1 )
   *   f( t2, t1 )
   *   f( t1, t2 )
   *   f( t1, t3 )
   *   f( t2, t2 )
   *   f( t3, t1 )
   *   ...
   * This function may add fewer than #num terms to terms if the enumeration is
   * exhausted, or if an internal error occurs.
   */
  void getEnumerateUfTerm( Node n, unsigned num, std::vector< Node >& terms );
  //
  void getEnumeratePredUfTerm( Node n, unsigned num, std::vector< Node >& terms );
  // uf operators enumerated
  std::map< Node, bool > d_uf_enum;
public:  //for property enumeration
  //process this candidate conjecture
  void processCandidateConjecture( TNode lhs, TNode rhs, unsigned lhs_depth, unsigned rhs_depth );
  //whether it should be considered, negative : no, positive returns score
  int considerCandidateConjecture( TNode lhs, TNode rhs );
  //notified of a substitution
  bool notifySubstitution( TNode glhs, std::map< TNode, TNode >& subs, TNode rhs );
  //confirmation count
  unsigned d_subs_confirmCount;
  //individual witnesses (for range)
  std::vector< TNode > d_subs_confirmWitnessRange;
  //individual witnesses (for domain)
  std::map< TNode, std::vector< TNode > > d_subs_confirmWitnessDomain;
  //number of ground substitutions whose equality is unknown
  unsigned d_subs_unkCount;
private:  //information about ground equivalence classes
  TNode d_bool_eqc[2];
  std::map< TNode, Node > d_ground_eqc_map;
  std::vector< TNode > d_ground_terms;
  //operator independent term index
  std::map< TNode, OpArgIndex > d_op_arg_index;
  //is handled term
  bool isHandledTerm( TNode n );
  Node getGroundEqc( TNode r );
  bool isGroundEqc( TNode r );
  bool isGroundTerm( TNode n );
  //has enumerated UF
  bool hasEnumeratedUf( Node n );
  // count of full effort checks
  unsigned d_fullEffortCount;
  // has added lemma
  bool d_hasAddedLemma;
  //flush the waiting conjectures
  unsigned flushWaitingConjectures( unsigned& addedLemmas, int ldepth, int rdepth );

 public:
  ConjectureGenerator(Env& env,
                      QuantifiersState& qs,
                      QuantifiersInferenceManager& qim,
                      QuantifiersRegistry& qr,
                      TermRegistry& tr);
  ~ConjectureGenerator();

  /* needs check */
  bool needsCheck(Theory::Effort e) override;
  /* reset at a round */
  void reset_round(Theory::Effort e) override;
  /* Call during quantifier engine's check */
  void check(Theory::Effort e, QEffort quant_e) override;
  /** Identify this module (for debugging, dynamic configuration, etc..) */
  std::string identify() const override;
  // options
 private:
  bool optReqDistinctVarPatterns();
  bool optFilterUnknown();
  int optFilterScoreThreshold();
  unsigned optFullCheckFrequency();
  unsigned optFullCheckConjectures();

  bool optStatsOnly();
  /** term canonizer */
  expr::TermCanonize d_termCanon;
};


}
}
}  // namespace cvc5::internal

#endif
