/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2010 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced
 * Research Projects Agency and the National Science Foundation of the
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/**
 * @file state_align_search.c State (and phone and word) alignment search.
 */

#include "state_align_search.h"

static int
state_align_search_start(ps_search_t *search)
{
    state_align_search_t *sas = (state_align_search_t *)search;

    /* Activate the initial state. */
    hmm_enter(sas->hmms, 0, 0, 0);

    return 0;
}

static void
renormalize_hmms(state_align_search_t *sas, int frame_idx, int32 norm)
{
    int i;
    (void) frame_idx;
    for (i = 0; i < sas->n_phones; ++i)
        hmm_normalize(sas->hmms + i, norm);
}

static int32
evaluate_hmms(state_align_search_t *sas, int16 const *senscr, int frame_idx)
{
    int32 bs = WORST_SCORE;
    int i;

    hmm_context_set_senscore(sas->hmmctx, senscr);

    for (i = 0; i < sas->n_phones; ++i) {
        hmm_t *hmm = sas->hmms + i;
        int32 score;

        if (hmm_frame(hmm) < frame_idx)
            continue;
        score = hmm_vit_eval(hmm);
        if (score BETTER_THAN bs) {
            bs = score;
        }
    }
    return bs;
}

static void
prune_hmms(state_align_search_t *sas, int frame_idx)
{
    int nf = frame_idx + 1;
    int i;

    /* Check all phones to see if they remain active in the next frame. */
    for (i = 0; i < sas->n_phones; ++i) {
        hmm_t *hmm = sas->hmms + i;
        if (hmm_frame(hmm) < frame_idx)
            continue;
        /* Enforce alignment constraint: due to non-emitting states,
         * previous phone's HMM remains active in first frame of its
         * successor. */
        if (nf > sas->ef[i])
            continue;
        hmm_frame(hmm) = nf;
    }
}

static void
phone_transition(state_align_search_t *sas, int frame_idx)
{
    int nf = frame_idx + 1;
    int i;

    for (i = 0; i < sas->n_phones - 1; ++i) {
        hmm_t *hmm, *nhmm;
        int32 newphone_score;

        hmm = sas->hmms + i;
        if (hmm_frame(hmm) != nf)
            continue;
        /* Enforce alignment constraint for initial state of each phone. */
        if (nf < sas->sf[i + 1])
            continue;

        newphone_score = hmm_out_score(hmm);
        /* Transition into next phone using the usual Viterbi rule. */
        nhmm = hmm + 1;
        if (hmm_frame(nhmm) < frame_idx
            || newphone_score BETTER_THAN hmm_in_score(nhmm)) {
            hmm_enter(nhmm, newphone_score, hmm_out_history(hmm), nf);
        }
    }
}

#define TOKEN_STEP 20
static void
extend_tokenstack(state_align_search_t *sas, int frame_idx)
{
    if (frame_idx >= sas->n_fr_alloc) {
        sas->n_fr_alloc = frame_idx + TOKEN_STEP + 1;
        sas->tokens = ckd_realloc(sas->tokens,
                                  sas->n_emit_state * sas->n_fr_alloc
                                  * sizeof(*sas->tokens));
    }
    memset(sas->tokens + frame_idx * sas->n_emit_state, 0xff,
           sas->n_emit_state * sizeof(*sas->tokens));
}

static void
record_transitions(state_align_search_t *sas, int frame_idx)
{
    state_align_hist_t *tokens;
    int i;

    /* Push another frame of tokens on the stack. */
    extend_tokenstack(sas, frame_idx);
    tokens = sas->tokens + frame_idx * sas->n_emit_state;

    /* Scan all active HMMs */
    for (i = 0; i < sas->n_phones; ++i) {
        hmm_t *hmm = sas->hmms + i;
        int j;

        if (hmm_frame(hmm) < frame_idx)
            continue;
        for (j = 0; j < sas->hmmctx->n_emit_state; ++j) {
            int state_idx = i * sas->hmmctx->n_emit_state + j;
            /* Record their backpointers on the token stack. */
            tokens[state_idx].id = hmm_history(hmm, j);
            tokens[state_idx].score = hmm_score(hmm, j);
            /* Update backpointer fields with state index. */
            hmm_history(hmm, j) = state_idx;
        }
    }
}

static int
state_align_search_step(ps_search_t *search, int frame_idx)
{
    state_align_search_t *sas = (state_align_search_t *)search;
    acmod_t *acmod = ps_search_acmod(search);
    int16 const *senscr;
    int i;

    /* Calculate senone scores. */
    for (i = 0; i < sas->n_phones; ++i)
        if (hmm_frame(&sas->hmms[i]) == frame_idx)
            acmod_activate_hmm(acmod, &sas->hmms[i]);
    senscr = acmod_score(acmod, &frame_idx);

    /* Renormalize here if needed. */
    /* FIXME: Make sure to (unit-)test this!!! */
    if ((sas->best_score - 0x300000) WORSE_THAN WORST_SCORE) {
        E_INFO("Renormalizing Scores at frame %d, best score %d\n",
               frame_idx, sas->best_score);
        renormalize_hmms(sas, frame_idx, sas->best_score);
    }

    /* Viterbi step. */
    sas->best_score = evaluate_hmms(sas, senscr, frame_idx);
    prune_hmms(sas, frame_idx);

    /* Transition out of non-emitting states. */
    phone_transition(sas, frame_idx);

    /* Generate new tokens from best path results. */
    record_transitions(sas, frame_idx);

    /* Update frame counter */
    sas->frame = frame_idx;

    return 0;
}

static int
state_align_search_finish(ps_search_t *search)
{
    state_align_search_t *sas = (state_align_search_t *)search;
    hmm_t *final_phone = sas->hmms + sas->n_phones - 1;
    ps_alignment_iter_t *itor;
    ps_alignment_entry_t *ent;

    int last_frame, cur_frame;
    state_align_hist_t last, cur;

    /* Best state exiting the last cur_frame. */
    last.id = cur.id = hmm_out_history(final_phone);
    last.score = hmm_out_score(final_phone);
    if (last.id == 0xffff) {
        E_ERROR("Failed to reach final state in alignment\n");
        return -1;
    }
    itor = ps_alignment_states(sas->al);
    last_frame = sas->frame + 1;
    for (cur_frame = sas->frame - 1; cur_frame >= 0; --cur_frame) {
	cur = sas->tokens[cur_frame * sas->n_emit_state + cur.id];
        /* State boundary, update alignment entry for next state. */
        if (cur.id != last.id) {
            itor = ps_alignment_iter_goto(itor, last.id);
            assert(itor != NULL);
            ent = ps_alignment_iter_get(itor);
            ent->start = cur_frame + 1;
            ent->duration = last_frame - ent->start;
            ent->score =  last.score - cur.score;
            E_DEBUG("state %d start %d end %d\n", last.id,
                    ent->start, last_frame);
    	    last = cur;
            last_frame = cur_frame + 1;
        }
    }
    /* Update alignment entry for initial state. */
    itor = ps_alignment_iter_goto(itor, 0);
    assert(itor != NULL);
    ent = ps_alignment_iter_get(itor);
    ent->start = 0;
    ent->duration = last_frame;
    E_DEBUG("state %d start %d end %d\n", 0,
            ent->start, last_frame);
    ps_alignment_iter_free(itor);
    ps_alignment_propagate(sas->al);

    return 0;
}

static int
state_align_search_reinit(ps_search_t *search, dict_t *dict, dict2pid_t *d2p)
{
    /* This does nothing, you need to make a new search for each utterance. */
    (void) search;
    (void) dict;
    (void) d2p;
    return 0;
}

static void
state_align_search_free(ps_search_t *search)
{
    state_align_search_t *sas = (state_align_search_t *)search;
    ps_search_base_free(search);
    ckd_free(sas->hmms);
    ckd_free(sas->tokens);
    ckd_free(sas->sf);
    ckd_free(sas->ef);
    hmm_context_free(sas->hmmctx);
    ps_alignment_free(sas->al);
    ckd_free(sas);
}

struct state_align_seg_s {
    ps_seg_t base;
    ps_alignment_iter_t *itor;
};
typedef struct state_align_seg_s state_align_seg_t;

static void
state_align_search_seg_free(ps_seg_t * seg)
{
    state_align_seg_t *itor = (state_align_seg_t *)seg;
    if (itor->itor != NULL) {
        /* If we hit the end of the alignment, it was already freed! */
        ps_alignment_iter_free(itor->itor);
    }
    ckd_free(itor);
}

static void
state_align_search_fill_iter(ps_seg_t *seg)
{
    state_align_seg_t *itor = (state_align_seg_t *)seg;
    ps_alignment_entry_t *entry = ps_alignment_iter_get(itor->itor);

    seg->sf = entry->start;
    seg->ef = entry->start + entry->duration - 1;
    seg->ascr = entry->score;
    seg->lscr = 0;
    seg->text = dict_wordstr(ps_search_dict(seg->search), entry->id.wid);
    seg->wid = entry->id.wid;
}

static ps_seg_t *
state_align_search_seg_next(ps_seg_t * seg)
{
    state_align_seg_t *itor = (state_align_seg_t *)seg;

    itor->itor = ps_alignment_iter_next(itor->itor);
    if (itor->itor == NULL) {
        state_align_search_seg_free(seg);
        return NULL;
    }
    state_align_search_fill_iter(seg);
    return seg;
}

static ps_segfuncs_t state_align_segfuncs = {
    /* seg_next */ state_align_search_seg_next,
    /* seg_free */ state_align_search_seg_free
};


static ps_seg_t *
state_align_search_seg_iter(ps_search_t * search)
{
    state_align_search_t *sas = (state_align_search_t *) search;
    state_align_seg_t *seg;
    ps_alignment_iter_t *itor;

    if (sas->al == NULL)
        return NULL;
    /* Even though the alignment has a bunch of levels, for the
       purposes of the decoder API we will just iterate over words,
       which is the most likely/useful use case.  We will also expose
       the rest of the alignment API separately. */

    itor = ps_alignment_words(sas->al);
    if (itor == NULL)
        return NULL;
    seg = ckd_calloc(1, sizeof(state_align_seg_t));
    seg->base.vt = &state_align_segfuncs;
    seg->base.search = search;
    seg->itor = itor;
    state_align_search_fill_iter((ps_seg_t *)seg);

    return (ps_seg_t *)seg;
}

static char const *
state_align_search_hyp(ps_search_t *search, int32 *out_score)
{
    state_align_search_t *sas = (state_align_search_t *)search;
    ps_alignment_iter_t *itor;
    size_t hyp_len;

    if (search->hyp_str)
        ckd_free(search->hyp_str);
    search->hyp_str = NULL;
    if (sas->al == NULL)
        return NULL;
    itor = ps_alignment_words(sas->al);
    if (itor == NULL)
        return NULL;
    for (hyp_len = 0; itor; itor = ps_alignment_iter_next(itor)) {
        const char *word;
        int32 wid = ps_alignment_iter_get(itor)->id.wid;

        if (dict_real_word(ps_search_dict(search), wid)) {
            word = dict_basestr(ps_search_dict(search),
                                ps_alignment_iter_get(itor)->id.wid);
            if (word == NULL) {
                E_ERROR("Unknown word id %d in alignment",
                        ps_alignment_iter_get(itor)->id.wid);
                return NULL;
            }
            hyp_len += strlen(word) + 1;
        }
    }
    search->hyp_str = ckd_calloc(hyp_len + 1, sizeof(*search->hyp_str));
    for (itor = ps_alignment_words(sas->al);
         itor; itor = ps_alignment_iter_next(itor)) {
        ps_alignment_entry_t *ent = ps_alignment_iter_get(itor);
        int32 wid = ent->id.wid;
        const char *word;
        if (dict_real_word(ps_search_dict(search), wid)) {
            word = dict_basestr(ps_search_dict(search),
                                ent->id.wid);
            strcat(search->hyp_str, word);
            strcat(search->hyp_str, " ");
        }
        *out_score = ent->score;
    }
    search->hyp_str[strlen(search->hyp_str) - 1] = '\0';
    return search->hyp_str;
}

static ps_searchfuncs_t state_align_search_funcs = {
    /* start: */  state_align_search_start,
    /* step: */   state_align_search_step,
    /* finish: */ state_align_search_finish,
    /* reinit: */ state_align_search_reinit,
    /* free: */   state_align_search_free,
    /* lattice: */  NULL,
    /* hyp: */      state_align_search_hyp,
    /* prob: */     NULL,
    /* seg_iter: */ state_align_search_seg_iter,
};

ps_search_t *
state_align_search_init(const char *name,
                        cmd_ln_t *config,
                        acmod_t *acmod,
                        ps_alignment_t *al)
{
    state_align_search_t *sas;
    ps_alignment_iter_t *itor;
    int i;

    sas = ckd_calloc(1, sizeof(*sas));
    ps_search_init(ps_search_base(sas), &state_align_search_funcs,
		   PS_SEARCH_TYPE_STATE_ALIGN, name,
                   config, acmod, al->d2p->dict, al->d2p);
    sas->hmmctx = hmm_context_init(bin_mdef_n_emit_state(acmod->mdef),
                                   acmod->tmat->tp, NULL, acmod->mdef->sseq);
    if (sas->hmmctx == NULL) {
        ckd_free(sas);
        return NULL;
    }
    sas->al = ps_alignment_retain(al);

    /* Generate HMM vector from phone level of alignment. */
    sas->n_phones = ps_alignment_n_phones(al);
    sas->n_emit_state = ps_alignment_n_states(al);
    sas->hmms = ckd_calloc(sas->n_phones, sizeof(*sas->hmms));
    sas->sf = ckd_calloc(sas->n_phones, sizeof(*sas->sf));
    sas->ef = ckd_calloc(sas->n_phones, sizeof(*sas->ef));
    for (i = 0, itor = ps_alignment_phones(al);
         i < sas->n_phones && itor;
         ++i, itor = ps_alignment_iter_next(itor)) {
        ps_alignment_entry_t *ent = ps_alignment_iter_get(itor);
        hmm_init(sas->hmmctx, &sas->hmms[i], FALSE,
                 ent->id.pid.ssid, ent->id.pid.tmatid);
        if (ent-> start > 0)
            sas->sf[i] = ent->start;
        else
            sas->sf[i] = 0; /* Always active */
        if (ent->duration > 0)
            sas->ef[i] = ent->start + ent->duration;
        else
            sas->ef[i] = INT_MAX; /* Always active */
    }
    return ps_search_base(sas);
}
