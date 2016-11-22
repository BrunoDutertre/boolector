/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015-2016 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "aigprop.h"
#include "btorclone.h"
#include "btorcore.h"
#include "utils/btorhashint.h"
#include "utils/btorhashptr.h"
#include "utils/btorstack.h"
#include "utils/btorutil.h"

#include <math.h>

/*------------------------------------------------------------------------*/

#define AIGPROPLOG(level, fmt, args...) \
  do                                    \
  {                                     \
    if (aprop->loglevel < level) break; \
    msg (fmt, ##args);                  \
  } while (0)

void
msg (char *fmt, ...)
{
  va_list msg;

  fputs ("[aigprop] ", stdout);
  va_start (msg, fmt);
  vfprintf (stdout, fmt, msg);
  va_end (msg);
  fputc ('\n', stdout);
  fflush (stdout);
}

/*------------------------------------------------------------------------*/

#define AIGPROP_MAXSTEPS_CFACT 100
#define AIGPROP_MAXSTEPS(i) \
  (AIGPROP_MAXSTEPS_CFACT * ((i) &1u ? 1 : 1 << ((i) >> 1)))

#define AIGPROP_SELECT_CFACT 20

/*------------------------------------------------------------------------*/

int
aigprop_get_assignment_aig (BtorPtrHashTable *model, BtorAIG *aig)
{
  assert (model);

  int res;

  if (BTOR_IS_TRUE_AIG (aig)) return 1;
  if (BTOR_IS_FALSE_AIG (aig)) return -1;

  assert (btor_get_ptr_hash_table (model, BTOR_REAL_ADDR_AIG (aig)));
  res = btor_get_ptr_hash_table (model, BTOR_REAL_ADDR_AIG (aig))->data.as_int;
  res = BTOR_IS_INVERTED_AIG (aig) ? -res : res;
  return res;
}

/*------------------------------------------------------------------------*/

/* score
 *
 * score (aigvar, A) = A (aigvar)
 * score (BTOR_CONST_AIG_TRUE, A) = 1.0
 * score (BTOR_CONST_AIG_FALSE, A) = 0.0
 * score (aig0 /\ aig1, A) = 1/2 * (score (aig0) + score (aig1), A)
 * score (-(-aig0 /\ -aig1), A) = max (score (-aig0), score (-aig1), A)
 */

#define AIGPROP_LOG_COMPUTE_SCORE_AIG(cur, left, right, s0, s1, res) \
  do                                                                 \
  {                                                                  \
    a = aigprop_get_assignment_aig (aprop->model, left);             \
    assert (a);                                                      \
    AIGPROPLOG (3,                                                   \
                "        assignment aig0 (%s%d): %d",                \
                BTOR_IS_INVERTED_AIG (left) ? "-" : "",              \
                BTOR_REAL_ADDR_AIG (left)->id,                       \
                a < 0 ? 0 : 1);                                      \
    a = aigprop_get_assignment_aig (aprop->model, right);            \
    assert (a);                                                      \
    AIGPROPLOG (3,                                                   \
                "        assignment aig1 (%s%d): %d",                \
                BTOR_IS_INVERTED_AIG (right) ? "-" : "",             \
                BTOR_REAL_ADDR_AIG (right)->id,                      \
                a < 0 ? 0 : 1);                                      \
    AIGPROPLOG (3,                                                   \
                "        score      aig0 (%s%d): %f%s",              \
                BTOR_IS_INVERTED_AIG (left) ? "-" : "",              \
                BTOR_REAL_ADDR_AIG (left)->id,                       \
                s0,                                                  \
                s0 < 1.0 ? " (< 1.0)" : "");                         \
    AIGPROPLOG (3,                                                   \
                "        score      aig1 (%s%d): %f%s",              \
                BTOR_IS_INVERTED_AIG (right) ? "-" : "",             \
                BTOR_REAL_ADDR_AIG (right)->id,                      \
                s1,                                                  \
                s1 < 1.0 ? " (< 1.0)" : "");                         \
    AIGPROPLOG (3,                                                   \
                "      * score cur (%s%d): %f%s",                    \
                BTOR_IS_INVERTED_AIG (cur) ? "-" : "",               \
                real_cur->id,                                        \
                res,                                                 \
                res < 1.0 ? " (< 1.0)" : "");                        \
  } while (0)

static double
compute_score_aig (AIGProp *aprop, BtorAIG *aig)
{
  assert (aprop);
  assert (!BTOR_IS_CONST_AIG (aig));

  double res, s0, s1;
  BtorPtrHashBucket *b;
  BtorAIGPtrStack stack;
  BtorAIG *cur, *real_cur, *left, *right;
  BtorIntHashTable *mark;
  BtorHashTableData *d;
  BtorMemMgr *mm;
#ifndef NDEBUG
  int a;
#endif

  if ((b = btor_get_ptr_hash_table (aprop->score, aig))) return b->data.as_dbl;

  mm  = aprop->amgr->btor->mm;
  res = 0.0;

  BTOR_INIT_STACK (stack);
  mark = btor_new_int_hash_map (mm);

  BTOR_PUSH_STACK (mm, stack, aig);
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_AIG (cur);

    if (BTOR_IS_CONST_AIG (real_cur)) continue;
    if (btor_get_ptr_hash_table (aprop->score, cur)) continue;
    d = btor_get_int_hash_map (mark, real_cur->id);
    if (d && d->as_int == 1) continue;

    if (!d)
    {
      btor_add_int_hash_map (mark, real_cur->id);
      assert (BTOR_IS_VAR_AIG (real_cur) || BTOR_IS_AND_AIG (real_cur));
      BTOR_PUSH_STACK (mm, stack, cur);
      if (BTOR_IS_AND_AIG (real_cur))
      {
        left  = btor_aig_get_left_child (aprop->amgr, real_cur);
        right = btor_aig_get_right_child (aprop->amgr, real_cur);
        if (!BTOR_IS_CONST_AIG (left)) BTOR_PUSH_STACK (mm, stack, left);
        if (!BTOR_IS_CONST_AIG (right)) BTOR_PUSH_STACK (mm, stack, right);
      }
    }
    else
    {
      assert (d->as_int == 0);
      d->as_int = 1;
      assert (aigprop_get_assignment_aig (aprop->model, cur) != 0);
#ifndef NDEBUG
      a = aigprop_get_assignment_aig (aprop->model, cur);
      assert (a);
      AIGPROPLOG (3, "");
      AIGPROPLOG (3,
                  "  ** assignment cur (%s%d): %d",
                  BTOR_IS_INVERTED_AIG (cur) ? "-" : "",
                  real_cur->id,
                  a < 0 ? 0 : 1);
#endif
      assert (!btor_get_ptr_hash_table (aprop->score, cur));
      assert (!btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (cur)));

      if (BTOR_IS_VAR_AIG (real_cur))
      {
        res = aigprop_get_assignment_aig (aprop->model, cur) < 0 ? 0.0 : 1.0;
        AIGPROPLOG (3,
                    "        * score cur (%s%d): %f",
                    BTOR_IS_INVERTED_AIG (cur) ? "-" : "",
                    real_cur->id,
                    res);
        AIGPROPLOG (3,
                    "        * score cur (%s%d): %f",
                    BTOR_IS_INVERTED_AIG (cur) ? "" : "-",
                    real_cur->id,
                    res == 0.0 ? 1.0 : 0.0);
        btor_add_ptr_hash_table (aprop->score, cur)->data.as_dbl = res;
        btor_add_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (cur))
            ->data.as_dbl = res == 0.0 ? 1.0 : 0.0;
      }
      else
      {
        assert (BTOR_IS_AND_AIG (real_cur));
        left  = btor_aig_get_left_child (aprop->amgr, real_cur);
        right = btor_aig_get_right_child (aprop->amgr, real_cur);
        assert (BTOR_IS_CONST_AIG (left)
                || btor_get_ptr_hash_table (aprop->score, left));
        assert (BTOR_IS_CONST_AIG (right)
                || btor_get_ptr_hash_table (aprop->score, right));
        assert (
            BTOR_IS_CONST_AIG (left)
            || btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (left)));
        assert (
            BTOR_IS_CONST_AIG (right)
            || btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (right)));

        s0 = BTOR_IS_CONST_AIG (left)
                 ? (left == BTOR_AIG_TRUE ? 1.0 : 0.0)
                 : btor_get_ptr_hash_table (aprop->score, left)->data.as_dbl;
        s1 = BTOR_IS_CONST_AIG (right)
                 ? (right == BTOR_AIG_TRUE ? 1.0 : 0.0)
                 : btor_get_ptr_hash_table (aprop->score, right)->data.as_dbl;
        res = (s0 + s1) / 2.0;
        /* fix rounding errors (eg. (0.999+1.0)/2 = 1.0) ->
           choose minimum (else it might again result in 1.0) */
        if (res == 1.0 && (s0 < 1.0 || s1 < 1.0)) res = s0 < s1 ? s0 : s1;
        assert (res >= 0.0 && res <= 1.0);
        btor_add_ptr_hash_table (aprop->score, real_cur)->data.as_dbl = res;
#ifndef NDEBUG
        AIGPROP_LOG_COMPUTE_SCORE_AIG (real_cur, left, right, s0, s1, res);
#endif
        s0 =
            BTOR_IS_CONST_AIG (left)
                ? (left == BTOR_AIG_TRUE ? 0.0 : 1.0)
                : btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (left))
                      ->data.as_dbl;
        s1 = BTOR_IS_CONST_AIG (right)
                 ? (right == BTOR_AIG_TRUE ? 0.0 : 1.0)
                 : btor_get_ptr_hash_table (aprop->score,
                                            BTOR_INVERT_AIG (right))
                       ->data.as_dbl;
        res = s0 > s1 ? s0 : s1;
        assert (res >= 0.0 && res <= 1.0);
        btor_add_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (real_cur))
            ->data.as_dbl = res;
#ifndef NDEBUG
        AIGPROP_LOG_COMPUTE_SCORE_AIG (BTOR_INVERT_AIG (real_cur),
                                       BTOR_INVERT_AIG (left),
                                       BTOR_INVERT_AIG (right),
                                       s0,
                                       s1,
                                       res);
#endif
      }
      assert (btor_get_ptr_hash_table (aprop->score, cur));
      assert (btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (cur)));
    }
  }

  btor_delete_int_hash_map (mark);
  BTOR_RELEASE_STACK (mm, stack);

  assert (btor_get_ptr_hash_table (aprop->score, aig));
  assert (btor_get_ptr_hash_table (aprop->score, BTOR_INVERT_AIG (aig)));
  return res;
}

static void
compute_scores (AIGProp *aprop)
{
  assert (aprop);
  assert (aprop->roots);
  assert (aprop->model);

  BtorAIGPtrStack stack;
  BtorIntHashTable *cache;
  BtorAIG *cur, *real_cur, *left, *right;
  BtorPtrHashTableIterator it;
  BtorMemMgr *mm;

  AIGPROPLOG (3, "*** compute scores");

  mm = aprop->amgr->btor->mm;

  BTOR_INIT_STACK (stack);
  cache = btor_new_int_hash_table (mm);

  if (!aprop->score)
    aprop->score =
        btor_new_ptr_hash_table (mm,
                                 (BtorHashPtr) btor_hash_aig_by_id,
                                 (BtorCmpPtr) btor_compare_aig_by_id);

  /* collect roots */
  btor_init_ptr_hash_table_iterator (&it, aprop->roots);
  while (btor_has_next_ptr_hash_table_iterator (&it))
    BTOR_PUSH_STACK (mm, stack, btor_next_ptr_hash_table_iterator (&it));

  /* compute scores */
  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_AIG (cur);

    if (BTOR_IS_CONST_AIG (real_cur)) continue;
    if (btor_get_ptr_hash_table (aprop->score, cur)) continue;

    if (!btor_contains_int_hash_table (cache, real_cur->id))
    {
      btor_add_int_hash_table (cache, real_cur->id);
      assert (BTOR_IS_VAR_AIG (real_cur) || BTOR_IS_AND_AIG (real_cur));
      BTOR_PUSH_STACK (mm, stack, cur);
      if (BTOR_IS_AND_AIG (real_cur))
      {
        left  = btor_aig_get_left_child (aprop->amgr, real_cur);
        right = btor_aig_get_right_child (aprop->amgr, real_cur);
        if (!BTOR_IS_CONST_AIG (left)
            && !btor_contains_int_hash_table (cache,
                                              BTOR_REAL_ADDR_AIG (left)->id))
          BTOR_PUSH_STACK (mm, stack, left);
        if (!BTOR_IS_CONST_AIG (right)
            && !btor_contains_int_hash_table (cache,
                                              BTOR_REAL_ADDR_AIG (right)->id))
          BTOR_PUSH_STACK (mm, stack, right);
      }
    }
    else
    {
      compute_score_aig (aprop, cur);
    }
  }

  /* cleanup */
  BTOR_RELEASE_STACK (mm, stack);
  btor_delete_int_hash_table (cache);
}

/*------------------------------------------------------------------------*/

static void
recursively_compute_assignment (AIGProp *aprop, BtorAIG *aig)
{
  assert (aprop);
  assert (aprop->model);
  assert (aig);

  int aleft, aright;
  BtorAIG *cur, *real_cur, *left, *right;
  BtorAIGPtrStack stack;
  BtorIntHashTable *cache;
  BtorMemMgr *mm;

  if (BTOR_IS_CONST_AIG (aig)) return;

  mm = aprop->amgr->btor->mm;

  cache = btor_new_int_hash_table (mm);
  BTOR_INIT_STACK (stack);
  BTOR_PUSH_STACK (mm, stack, aig);

  while (!BTOR_EMPTY_STACK (stack))
  {
    cur      = BTOR_POP_STACK (stack);
    real_cur = BTOR_REAL_ADDR_AIG (cur);
    assert (!BTOR_IS_CONST_AIG (real_cur));
    if (btor_get_ptr_hash_table (aprop->model, real_cur)) continue;

    if (BTOR_IS_VAR_AIG (real_cur))
    {
      /* initialize with false */
      btor_add_ptr_hash_table (aprop->model, real_cur)->data.as_int = -1;
    }
    else
    {
      assert (BTOR_IS_AND_AIG (real_cur));
      left  = btor_aig_get_left_child (aprop->amgr, real_cur);
      right = btor_aig_get_right_child (aprop->amgr, real_cur);

      if (!btor_contains_int_hash_table (cache, real_cur->id))
      {
        btor_add_int_hash_table (cache, real_cur->id);
        BTOR_PUSH_STACK (mm, stack, cur);
        if (!BTOR_IS_CONST_AIG (left)
            && !btor_contains_int_hash_table (cache,
                                              BTOR_REAL_ADDR_AIG (left)->id))
          BTOR_PUSH_STACK (mm, stack, left);
        if (!BTOR_IS_CONST_AIG (right)
            && !btor_contains_int_hash_table (cache,
                                              BTOR_REAL_ADDR_AIG (right)->id))
          BTOR_PUSH_STACK (mm, stack, right);
      }
      else
      {
        aleft = aigprop_get_assignment_aig (aprop->model, left);
        assert (aleft);
        aright = aigprop_get_assignment_aig (aprop->model, right);
        assert (aright);
        if (aleft < 0 || aright < 0)
          btor_add_ptr_hash_table (aprop->model, real_cur)->data.as_int = -1;
        else
          btor_add_ptr_hash_table (aprop->model, real_cur)->data.as_int = 1;
      }
    }
  }

  btor_delete_int_hash_table (cache);
  BTOR_RELEASE_STACK (mm, stack);
}

void
aigprop_delete_model (AIGProp *aprop)
{
  assert (aprop);

  if (!aprop->model) return;
  btor_delete_ptr_hash_table (aprop->model);
  aprop->model = 0;
}

void
aigprop_init_model (AIGProp *aprop)
{
  assert (aprop);

  if (aprop->model) aigprop_delete_model (aprop);
  aprop->model = btor_new_ptr_hash_table (aprop->amgr->btor->mm,
                                          (BtorHashPtr) btor_hash_aig_by_id,
                                          (BtorCmpPtr) btor_compare_aig_by_id);
}

void
aigprop_generate_model (AIGProp *aprop, int reset)
{
  assert (aprop);
  assert (aprop->roots);

  BtorPtrHashTableIterator it;

  if (reset) aigprop_init_model (aprop);

  btor_init_ptr_hash_table_iterator (&it, aprop->roots);
  while (btor_has_next_ptr_hash_table_iterator (&it))
    recursively_compute_assignment (aprop,
                                    btor_next_ptr_hash_table_iterator (&it));
}

/*------------------------------------------------------------------------*/

static inline void
update_unsatroots_table (AIGProp *aprop, BtorAIG *aig, int assignment)
{
  assert (aprop);
  assert (aig);
  assert (!BTOR_IS_CONST_AIG (aig));
  assert (btor_get_ptr_hash_table (aprop->roots, aig)
          || btor_get_ptr_hash_table (aprop->roots, BTOR_INVERT_AIG (aig)));
  assert (aigprop_get_assignment_aig (aprop->model, aig) != assignment);
  assert (assignment == 1 || assignment == -1);

  uint32_t id;

  id = btor_aig_get_id (aig);

  if (btor_contains_int_hash_map (aprop->unsatroots, id))
  {
    btor_remove_int_hash_map (aprop->unsatroots, id, 0);
    assert (aigprop_get_assignment_aig (aprop->model, aig) == -1);
    assert (assignment == 1);
  }
  else if (btor_contains_int_hash_map (aprop->unsatroots, -id))
  {
    btor_remove_int_hash_map (aprop->unsatroots, -id, 0);
    assert (aigprop_get_assignment_aig (aprop->model, BTOR_INVERT_AIG (aig))
            == -1);
    assert (assignment == -1);
  }
  else if (assignment == -1)
  {
    btor_add_int_hash_map (aprop->unsatroots, id);
    assert (aigprop_get_assignment_aig (aprop->model, aig) == 1);
  }
  else
  {
    btor_add_int_hash_map (aprop->unsatroots, -id);
    assert (aigprop_get_assignment_aig (aprop->model, BTOR_INVERT_AIG (aig))
            == 1);
  }
}

static void
update_cone (AIGProp *aprop, BtorAIG *aig, int assignment)
{
  assert (aprop);
  assert (aig);
  assert (BTOR_IS_REGULAR_AIG (aig));
  assert (BTOR_IS_VAR_AIG (aig));
  assert (assignment == 1 || assignment == -1);

  int aleft, aright, ass;
  uint32_t i;
  double start, delta;
  BtorIntHashTable *cache, *tmpcone;
  BtorIntHashTableIterator it;
  BtorPtrHashTableIterator pit;
  BtorPtrHashBucket *b;
  BtorHashTableData *d;
  BtorAIGPtrStack stack, cone;
  BtorAIG *cur, *left, *right, *child;
  BtorMemMgr *mm;

  start = btor_time_stamp ();

  mm = aprop->amgr->btor->mm;

#ifndef NDEBUG
  BtorAIG *root;
  btor_init_ptr_hash_table_iterator (&pit, aprop->roots);
  while (btor_has_next_ptr_hash_table_iterator (&pit))
  {
    root = btor_next_ptr_hash_table_iterator (&pit);
    assert (!BTOR_IS_FALSE_AIG (root));
    if ((!BTOR_IS_INVERTED_AIG (root)
         && aigprop_get_assignment_aig (aprop->model, BTOR_REAL_ADDR_AIG (root))
                == -1)
        || (BTOR_IS_INVERTED_AIG (root)
            && aigprop_get_assignment_aig (aprop->model,
                                           BTOR_REAL_ADDR_AIG (root))
                   == 1))
    {
      assert (btor_contains_int_hash_map (aprop->unsatroots,
                                          btor_aig_get_id (root)));
    }
    else if ((!BTOR_IS_INVERTED_AIG (root)
              && aigprop_get_assignment_aig (aprop->model,
                                             BTOR_REAL_ADDR_AIG (root))
                     == 1)
             || (BTOR_IS_INVERTED_AIG (root)
                 && aigprop_get_assignment_aig (aprop->model,
                                                BTOR_REAL_ADDR_AIG (root))
                        == -1))
    {
      assert (!btor_contains_int_hash_map (aprop->unsatroots,
                                           btor_aig_get_id (root)));
    }
  }
#endif

  /* reset cone ----------------------------------------------------------- */

  BTOR_INIT_STACK (cone);
  BTOR_INIT_STACK (stack);
  cache   = btor_new_int_hash_map (mm);
  tmpcone = btor_new_int_hash_table (mm);
  btor_add_int_hash_table (tmpcone, btor_aig_get_id (aig));

  btor_init_ptr_hash_table_iterator (&pit, aprop->roots);
  while (btor_has_next_ptr_hash_table_iterator (&pit))
  {
    cur = btor_next_ptr_hash_table_iterator (&pit);
    assert (!BTOR_IS_CONST_AIG (cur));
    BTOR_PUSH_STACK (mm, stack, cur);
  }

  while (!BTOR_EMPTY_STACK (stack))
  {
    cur = BTOR_REAL_ADDR_AIG (BTOR_POP_STACK (stack));
    assert (!BTOR_IS_CONST_AIG (cur));

    if ((d = btor_get_int_hash_map (cache, cur->id)) && d->as_int == 1)
      continue;

    if (!d)
    {
      d = btor_add_int_hash_map (cache, cur->id);
      if (BTOR_IS_VAR_AIG (cur))
      {
        d->as_int = 1;
        continue;
      }
      BTOR_PUSH_STACK (mm, stack, cur);
      for (i = 0; i < 2; i++)
      {
        child = btor_aig_get_by_id (aprop->amgr, cur->children[i]);
        if (!BTOR_IS_CONST_AIG (child)) BTOR_PUSH_STACK (mm, stack, child);
      }
    }
    else
    {
      assert (btor_get_ptr_hash_table (aprop->model, cur));
      assert (d->as_int == 0);
      d->as_int = 1;
      for (i = 0; i < 2; i++)
      {
        child = BTOR_REAL_ADDR_AIG (
            btor_aig_get_by_id (aprop->amgr, cur->children[i]));
        if (BTOR_IS_CONST_AIG (child)) continue;
        if (btor_contains_int_hash_table (tmpcone, btor_aig_get_id (child))
            && !btor_contains_int_hash_table (tmpcone, cur->id))
        {
          btor_add_int_hash_table (tmpcone, cur->id);
          BTOR_PUSH_STACK (mm, cone, cur);
          break;
        }
      }
    }
  }

  BTOR_RELEASE_STACK (mm, stack);
  btor_delete_int_hash_map (cache);
  btor_delete_int_hash_table (tmpcone);

  aprop->time.update_cone_reset += btor_time_stamp () - start;

  /* update assignment and score of 'aig' --------------------------------- */
  /* update model */
  b = btor_get_ptr_hash_table (aprop->model, aig);
  /* update unsatroots table */
  if (b->data.as_int != assignment
      && (btor_get_ptr_hash_table (aprop->roots, aig)
          || btor_get_ptr_hash_table (aprop->roots, BTOR_INVERT_AIG (aig))))
    update_unsatroots_table (aprop, aig, assignment);
  b->data.as_int = assignment;

  /* update score */
  if (aprop->score)
  {
    b              = btor_get_ptr_hash_table (aprop->score, aig);
    b->data.as_dbl = assignment < 0 ? 0.0 : 1.0;
  }

  qsort (cone.start,
         BTOR_COUNT_STACK (cone),
         sizeof (BtorAIG *),
         btor_compare_aig_by_id_qsort_asc);

  /* update model of cone ------------------------------------------------- */

  delta = btor_time_stamp ();

  for (i = 0; i < BTOR_COUNT_STACK (cone); i++)
  {
    cur = BTOR_PEEK_STACK (cone, i);
    assert (BTOR_IS_REGULAR_AIG (cur));
    assert (BTOR_IS_AND_AIG (cur));
    assert (btor_get_ptr_hash_table (aprop->model, cur));

    left  = btor_aig_get_left_child (aprop->amgr, cur);
    right = btor_aig_get_right_child (aprop->amgr, cur);
    aleft = aigprop_get_assignment_aig (aprop->model, left);
    assert (aleft);
    aright = aigprop_get_assignment_aig (aprop->model, right);
    assert (aright);
    ass = aleft < 0 || aright < 0 ? -1 : 1;
    b   = btor_get_ptr_hash_table (aprop->model, cur);
    /* update unsatroots table */
    if (b->data.as_int != ass
        && (btor_get_ptr_hash_table (aprop->roots, cur)
            || btor_get_ptr_hash_table (aprop->roots, BTOR_INVERT_AIG (cur))))
      update_unsatroots_table (aprop, cur, ass);
    b->data.as_int = ass;
  }

  aprop->time.update_cone_model_gen += btor_time_stamp () - delta;
  delta = btor_time_stamp ();

  /* update score of cone ------------------------------------------------- */

  if (aprop->score)
  {
    delta = btor_time_stamp ();
    for (i = 0; i < BTOR_COUNT_STACK (cone); i++)
    {
      cur = BTOR_PEEK_STACK (cone, i);
      assert (BTOR_IS_REGULAR_AIG (cur));
      assert (BTOR_IS_AND_AIG (cur));
      assert (btor_get_ptr_hash_table (aprop->score, cur));

      b = btor_get_ptr_hash_table (aprop->score, cur);
      b->data.as_dbl =
          aigprop_get_assignment_aig (aprop->model, cur) < 0 ? 0.0 : 1.0;
    }
    aprop->time.update_cone_compute_score += btor_time_stamp () - delta;
  }

  BTOR_RELEASE_STACK (mm, cone);

#ifndef NDEBUG
  btor_init_ptr_hash_table_iterator (&pit, aprop->roots);
  while (btor_has_next_ptr_hash_table_iterator (&pit))
  {
    root = btor_next_ptr_hash_table_iterator (&pit);
    if ((!BTOR_IS_INVERTED_AIG (root)
         && aigprop_get_assignment_aig (aprop->model, BTOR_REAL_ADDR_AIG (root))
                == -1)
        || (BTOR_IS_INVERTED_AIG (root)
            && aigprop_get_assignment_aig (aprop->model,
                                           BTOR_REAL_ADDR_AIG (root))
                   == 1))
    {
      assert (btor_contains_int_hash_map (aprop->unsatroots,
                                          btor_aig_get_id (root)));
    }
    else if ((!BTOR_IS_INVERTED_AIG (root)
              && aigprop_get_assignment_aig (aprop->model,
                                             BTOR_REAL_ADDR_AIG (root))
                     == 1)
             || (BTOR_IS_INVERTED_AIG (root)
                 && aigprop_get_assignment_aig (aprop->model,
                                                BTOR_REAL_ADDR_AIG (root))
                        == -1))
    {
      assert (!btor_contains_int_hash_map (aprop->unsatroots,
                                           btor_aig_get_id (root)));
    }
  }
#endif

  aprop->time.update_cone += btor_time_stamp () - start;
}

/*------------------------------------------------------------------------*/

static BtorAIG *
select_root (AIGProp *aprop, uint32_t nmoves)
{
  assert (aprop);
  assert (aprop->unsatroots);
  assert (aprop->score);

  BtorAIG *res, *cur;
  BtorIntHashTableIterator it;
  BtorMemMgr *mm;

  mm  = aprop->amgr->btor->mm;
  res = 0;

  if (aprop->use_bandit)
  {
    int *selected;
    double value, max_value, score;
    BtorPtrHashBucket *b;

    max_value = 0.0;
    btor_init_int_hash_table_iterator (&it, aprop->unsatroots);
    while (btor_has_next_int_hash_table_iterator (&it))
    {
      selected = &aprop->unsatroots->data[it.cur_pos].as_int;
      cur      = btor_aig_get_by_id (aprop->amgr,
                                btor_next_int_hash_table_iterator (&it));
      assert (aigprop_get_assignment_aig (aprop->model, cur) != 1);
      assert (!BTOR_IS_CONST_AIG (cur));
      b = btor_get_ptr_hash_table (aprop->score, cur);
      assert (b);
      score = b->data.as_dbl;
      assert (score < 1.0);
      if (!res)
      {
        res = cur;
        *selected += 1;
        continue;
      }
      value = score + AIGPROP_SELECT_CFACT * sqrt (log (*selected) / nmoves);
      if (value > max_value)
      {
        res       = cur;
        max_value = value;
        *selected *= 1;
      }
    }
  }
  else
  {
    uint32_t r;
    BtorAIGPtrStack stack;
    BTOR_INIT_STACK (stack);
    btor_init_int_hash_table_iterator (&it, aprop->unsatroots);
    while (btor_has_next_int_hash_table_iterator (&it))
    {
      cur = btor_aig_get_by_id (aprop->amgr,
                                btor_next_int_hash_table_iterator (&it));
      assert (aigprop_get_assignment_aig (aprop->model, cur) != 1);
      assert (!BTOR_IS_CONST_AIG (cur));
      BTOR_PUSH_STACK (mm, stack, cur);
    }
    assert (BTOR_COUNT_STACK (stack));
    r   = btor_pick_rand_rng (&aprop->rng, 0, BTOR_COUNT_STACK (stack) - 1);
    res = stack.start[r];
    BTOR_RELEASE_STACK (mm, stack);
  }

  assert (res);

  AIGPROPLOG (1, "");
  AIGPROPLOG (1,
              "*** select root: %s%d",
              BTOR_IS_INVERTED_AIG (res) ? "-" : "",
              BTOR_REAL_ADDR_AIG (res)->id);
  return res;
}

static void
select_move (AIGProp *aprop, BtorAIG *root, BtorAIG **input, int *assignment)
{
  assert (aprop);
  assert (root);
  assert (!BTOR_IS_CONST_AIG (root));
  assert (input);
  assert (assignment);

  int i, asscur, ass[2], assnew, eidx;
  BtorAIG *cur, *real_cur, *c[2];
  BtorPtrHashBucket *b;

  *input      = 0;
  *assignment = 0;

  cur    = root;
  asscur = 1;

  // printf ("----\n");
  // printf ("cur %s%d ass %d\n", BTOR_IS_INVERTED_AIG (cur) ? "-" : "",
  // BTOR_REAL_ADDR_AIG (cur)->id, btor_get_ptr_hash_table (aprop->model,
  // BTOR_REAL_ADDR_AIG (cur))->data.as_int);
  if (BTOR_IS_VAR_AIG (BTOR_REAL_ADDR_AIG (cur)))
  {
    *input      = BTOR_REAL_ADDR_AIG (cur);
    *assignment = BTOR_IS_INVERTED_AIG (cur) ? -asscur : asscur;
  }
  else
  {
    for (;;)
    {
      real_cur = BTOR_REAL_ADDR_AIG (cur);
      assert (BTOR_IS_AND_AIG (real_cur));
      asscur = BTOR_IS_INVERTED_AIG (cur) ? -asscur : asscur;
      // printf ("cur %s%d ass %d\n", BTOR_IS_INVERTED_AIG (cur) ? "-" : "",
      // BTOR_REAL_ADDR_AIG (cur)->id, btor_get_ptr_hash_table (aprop->model,
      // BTOR_REAL_ADDR_AIG (cur))->data.as_int);  printf ("asscur %d\n",
      // asscur);
      c[0] = btor_aig_get_by_id (aprop->amgr, real_cur->children[0]);
      c[1] = btor_aig_get_by_id (aprop->amgr, real_cur->children[1]);
      // printf ("c[0] %s%d ass %d\n", BTOR_IS_INVERTED_AIG (c[0]) ? "-" : "",
      // BTOR_REAL_ADDR_AIG (c[0])->id, BTOR_IS_INVERTED_AIG (c[0]) ?
      // -btor_get_ptr_hash_table (aprop->model, BTOR_REAL_ADDR_AIG
      // (c[0]))->data.as_int : btor_get_ptr_hash_table (aprop->model,
      // BTOR_REAL_ADDR_AIG (c[0]))->data.as_int);  printf ("c[1] %s%d ass
      // %d\n", BTOR_IS_INVERTED_AIG (c[1]) ? "-" : "", BTOR_REAL_ADDR_AIG
      // (c[1])->id, BTOR_IS_INVERTED_AIG (c[1]) ? -btor_get_ptr_hash_table
      // (aprop->model, BTOR_REAL_ADDR_AIG (c[1]))->data.as_int :
      // btor_get_ptr_hash_table (aprop->model, BTOR_REAL_ADDR_AIG
      // (c[1]))->data.as_int);

      /* conflict */
      if (BTOR_IS_AND_AIG (real_cur) && BTOR_IS_CONST_AIG (c[0])
          && BTOR_IS_CONST_AIG (c[1]))
        break;

      /* select path and determine path assignment */
      if (BTOR_IS_CONST_AIG (c[0]))
        eidx = 1;
      else if (BTOR_IS_CONST_AIG (c[1]))
        eidx = 0;
      else
      {
        /* choose 0-branch if exactly one branch is 0,
         * else choose randomly */
        for (i = 0; i < 2; i++)
        {
          assert (btor_get_ptr_hash_table (aprop->model,
                                           BTOR_REAL_ADDR_AIG (c[i])));
          b = btor_get_ptr_hash_table (aprop->model, BTOR_REAL_ADDR_AIG (c[i]));
          ass[i] =
              BTOR_IS_INVERTED_AIG (c[i]) ? -b->data.as_int : b->data.as_int;
        }
        if (ass[0] == -1 && ass[1] == 1)
          eidx = 0;
        else if (ass[0] == 1 && ass[1] == -1)
          eidx = 1;
        else
          eidx = btor_pick_rand_rng (&aprop->rng, 0, 1);
      }
      assert (eidx >= 0);
      // printf ("eidx %d\n", eidx);
      if (asscur == 1)
        assnew = 1;
      else if (ass[eidx ? 0 : 1] == 1)
        assnew = -1;
      else
      {
        assnew = btor_pick_rand_rng (&aprop->rng, 0, 1);
        if (!assnew) assnew = -1;
      }
      // printf ("assnew %d\n", assnew);

      cur    = c[eidx];
      asscur = assnew;

      if (BTOR_IS_VAR_AIG (BTOR_REAL_ADDR_AIG (cur)))
      {
        *input      = BTOR_REAL_ADDR_AIG (cur);
        *assignment = BTOR_IS_INVERTED_AIG (cur) ? -asscur : asscur;
        break;
      }
    }
  }
}

static int
move (AIGProp *aprop, uint32_t nmoves)
{
  assert (aprop);
  assert (aprop->roots);
  assert (aprop->unsatroots);
  assert (aprop->model);

  int assignment;
  BtorAIG *root, *input;

  /* roots contain false AIG -> unsat */
  if (!(root = select_root (aprop, nmoves))) return 0;

  select_move (aprop, root, &input, &assignment);

  AIGPROPLOG (1, "");
  AIGPROPLOG (1, "*** move");
#ifndef NDEBUG
  int a = aigprop_get_assignment_aig (aprop->model, input);
  AIGPROPLOG (1,
              "    * input: %s%d",
              BTOR_IS_INVERTED_AIG (input) ? "-" : "",
              BTOR_REAL_ADDR_AIG (input)->id);
  AIGPROPLOG (1, "      prev. assignment: %d", a);
  AIGPROPLOG (1, "      new   assignment: %d", assignment);
#endif

  update_cone (aprop, input, assignment);
  aprop->stats.moves += 1;
  return 1;
}

/*------------------------------------------------------------------------*/

// TODO termination callback?
int
aigprop_sat (AIGProp *aprop, BtorPtrHashTable *roots)
{
  assert (aprop);
  assert (roots);

  double start;
  int j, max_steps, sat_result;
  uint32_t nmoves;
  BtorMemMgr *mm;
  BtorPtrHashTableIterator it;
  BtorAIG *root;

  start      = btor_time_stamp ();
  sat_result = AIGPROP_UNKNOWN;
  nmoves     = 0;

  mm           = aprop->amgr->btor->mm;
  aprop->roots = roots;

  /* generate initial model, all inputs are initialized with false */
  aigprop_generate_model (aprop, 1);

  for (;;)
  {
    /* collect unsatisfied roots (kept up-to-date in update_cone) */
    assert (!aprop->unsatroots);
    aprop->unsatroots = btor_new_int_hash_map (mm);
    btor_init_ptr_hash_table_iterator (&it, roots);
    while (btor_has_next_ptr_hash_table_iterator (&it))
    {
      root = btor_next_ptr_hash_table_iterator (&it);
      if (BTOR_IS_TRUE_AIG (root)) continue;
      if (BTOR_IS_FALSE_AIG (root)) goto UNSAT;
      assert (aigprop_get_assignment_aig (aprop->model, root));
      if (!btor_contains_int_hash_map (aprop->unsatroots,
                                       btor_aig_get_id (root))
          && aigprop_get_assignment_aig (aprop->model, root) == -1)
        btor_add_int_hash_map (aprop->unsatroots, btor_aig_get_id (root));
    }

    /* compute initial score */
    compute_scores (aprop);

    if (!aprop->unsatroots->count) goto SAT;

    for (j = 0, max_steps = AIGPROP_MAXSTEPS (aprop->stats.restarts + 1);
         !aprop->use_restarts || j < max_steps;
         j++)
    {
      if (!(move (aprop, nmoves))) goto UNSAT;
      nmoves += 1;
      if (!aprop->unsatroots->count) goto SAT;
    }

    /* restart */
    aigprop_generate_model (aprop, 1);
    btor_delete_ptr_hash_table (aprop->score);
    aprop->score = 0;
    btor_delete_int_hash_map (aprop->unsatroots);
    aprop->unsatroots = 0;
    aprop->stats.restarts += 1;
  }
SAT:
  sat_result = AIGPROP_SAT;
  goto DONE;
UNSAT:
  sat_result = AIGPROP_UNSAT;
DONE:
  if (aprop->unsatroots) btor_delete_int_hash_map (aprop->unsatroots);
  aprop->unsatroots = 0;
  if (aprop->score) btor_delete_ptr_hash_table (aprop->score);
  aprop->score = 0;

  aprop->time.sat += btor_time_stamp () - start;
  return sat_result;
}

static void *
clone_key_as_aig (BtorMemMgr *mm, const void *cloned_amgr, const void *key)
{
  assert (cloned_amgr);
  assert (key);

  BtorAIG *aig, *cloned_aig;
  BtorAIGMgr *camgr;

  (void) mm;

  aig        = (BtorAIG *) key;
  camgr      = (BtorAIGMgr *) cloned_amgr;
  cloned_aig = btor_aig_get_by_id (camgr, BTOR_REAL_ADDR_AIG (aig)->id);
  assert (cloned_aig);
  if (BTOR_IS_INVERTED_AIG (aig)) cloned_aig = BTOR_INVERT_AIG (cloned_aig);
  return cloned_aig;
}

AIGProp *
aigprop_clone_aigprop (BtorAIGMgr *clone, AIGProp *aprop)
{
  assert (clone);

  AIGProp *res;
  BtorMemMgr *mm;

  if (!aprop) return 0;

  mm = clone->btor->mm;

  BTOR_CNEW (mm, res);
  memcpy (res, aprop, sizeof (AIGProp));
  res->amgr  = clone;
  res->roots = btor_clone_ptr_hash_table (
      mm, aprop->roots, clone_key_as_aig, btor_clone_data_as_int, clone, 0);
  res->unsatroots = btor_clone_int_hash_map (
      mm, aprop->unsatroots, btor_clone_data_as_int, 0);
  res->score = btor_clone_ptr_hash_table (
      mm, aprop->score, clone_key_as_aig, btor_clone_data_as_dbl, clone, 0);
  res->model = btor_clone_ptr_hash_table (
      mm, aprop->model, clone_key_as_aig, btor_clone_data_as_int, clone, 0);
  return res;
}

AIGProp *
aigprop_new_aigprop (BtorAIGMgr *amgr,
                     uint32_t seed,
                     uint32_t use_restarts,
                     uint32_t use_bandit)
{
  assert (amgr);

  AIGProp *res;

  BTOR_CNEW (amgr->btor->mm, res);
  res->amgr = amgr;
  btor_init_rng (&res->rng, seed);
  res->seed         = seed;
  res->use_restarts = use_restarts;
  res->use_bandit   = use_bandit;

  return res;
}

void
aigprop_delete_aigprop (AIGProp *aprop)
{
  assert (aprop);

  if (aprop->unsatroots) btor_delete_int_hash_map (aprop->unsatroots);
  if (aprop->score) btor_delete_ptr_hash_table (aprop->score);
  if (aprop->model) btor_delete_ptr_hash_table (aprop->model);
  BTOR_DELETE (aprop->amgr->btor->mm, aprop);
}

void
aigprop_print_stats (AIGProp *aprop)
{
  assert (aprop);
  msg ("");
  msg ("restarts: %d", aprop->stats.restarts);
  msg ("moves: %d", aprop->stats.moves);
}

void
aigprop_print_time_stats (AIGProp *aprop)
{
  assert (aprop);
  msg ("");
  msg ("%.2f seconds for sat call (AIG propagation)", aprop->time.sat);
}
