/*
 * Copyright (c) 2009, Rambler media
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Rambler media ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/***MODULE:chartable
 * rspamd module that make marks based on symbol chains
 */

#include "../config.h"
#include "../main.h"
#include "../message.h"
#include "../modules.h"
#include "../cfg_file.h"
#include "../expressions.h"
#include "../view.h"

#define DEFAULT_SYMBOL "R_CHARSET_MIXED"
#define DEFAULT_THRESHOLD 0.1

struct chartable_ctx {
	int (*header_filter)(struct worker_task *task);
	int (*mime_filter)(struct worker_task *task);
	int (*message_filter)(struct worker_task *task);
	int (*url_filter)(struct worker_task *task);
	char *metric;
	char *symbol;
	double threshold;

	memory_pool_t *chartable_pool;
};

static struct chartable_ctx *chartable_module_ctx = NULL;

static int chartable_mime_filter (struct worker_task *task);

int
chartable_module_init (struct config_file *cfg, struct module_ctx **ctx)
{
	chartable_module_ctx = g_malloc (sizeof (struct chartable_ctx));

	chartable_module_ctx->header_filter = NULL;
	chartable_module_ctx->mime_filter = chartable_mime_filter;
	chartable_module_ctx->message_filter = NULL;
	chartable_module_ctx->url_filter = NULL;
	chartable_module_ctx->chartable_pool = memory_pool_new (memory_pool_get_size ());

	*ctx = (struct module_ctx *)chartable_module_ctx;
	
	return 0;
}


int
chartable_module_config (struct config_file *cfg)
{
	char *value;
	int res = TRUE;

	if ((value = get_module_opt (cfg, "chartable", "metric")) != NULL) {
		chartable_module_ctx->metric = memory_pool_strdup (chartable_module_ctx->chartable_pool, value);
		g_free (value);
	}
	else {
		chartable_module_ctx->metric = DEFAULT_METRIC;
	}
	if ((value = get_module_opt (cfg, "chartable", "symbol")) != NULL) {
		chartable_module_ctx->symbol = memory_pool_strdup (chartable_module_ctx->chartable_pool, value);
		g_free (value);
	}
	else {
		chartable_module_ctx->symbol = DEFAULT_SYMBOL;
	}
	if ((value = get_module_opt (cfg, "chartable", "threshold")) != NULL) {
		errno = 0;
		chartable_module_ctx->threshold = strtod (value, NULL);
		if (errno != 0) {
			msg_warn ("chartable_module_config: invalid numeric value '%s': %s", value, strerror (errno));
			chartable_module_ctx->threshold = DEFAULT_THRESHOLD;
		}
	}
	else {
		chartable_module_ctx->threshold = DEFAULT_THRESHOLD;
	}
	
	return res;
}

int
chartable_module_reconfig (struct config_file *cfg)
{
	memory_pool_delete (chartable_module_ctx->chartable_pool);
	chartable_module_ctx->chartable_pool = memory_pool_new (1024);

	return chartable_module_config (cfg);
}

static gboolean
check_part (struct mime_text_part *part, gboolean raw_mode)
{
	unsigned char *p, *p1;
	gunichar c, t;
	GUnicodeScript scc, sct;
	uint32_t mark = 0, total = 0;
	uint32_t remain = part->content->len;
	
	p = part->content->data;

	if (part->is_raw || raw_mode) {
		while (remain > 1) {
			if ((g_ascii_isalpha (*p) && (*(p + 1) & 0x80)) ||
			   ((*p & 0x80) && g_ascii_isalpha (*(p + 1)))) {
				mark ++;
				total ++;
			}
			/* Current and next symbols are of one class */
			else if (((*p & 0x80) && (*(p + 1) & 0x80)) ||
					(g_ascii_isalpha (*p) && g_ascii_isalpha (*(p + 1)))) {
				total ++;		
			}
			p ++;
			remain --;
		}
	}
	else {
		while (remain > 0) {
			c = g_utf8_get_char_validated (p, remain);
			if (c == (gunichar)-2 || c == (gunichar)-1) {
				/* Invalid characters detected, stop processing*/
				return FALSE;
			}

			scc = g_unichar_get_script (c);
			p1 = g_utf8_next_char (p);
			remain -= p1 - p;
			p = p1;
			
			if (remain > 0) {
				t = g_utf8_get_char_validated (p, remain);
				if (c == (gunichar)-2 || c == (gunichar)-1) {
					/* Invalid characters detected, stop processing*/
					return FALSE;
				}
				sct = g_unichar_get_script (t);
				if (g_unichar_isalnum (c) && g_unichar_isalnum (t)) {
					/* We have two unicode alphanumeric characters, so we can check its script */
					if (sct != scc) {
						mark ++;
					}
					total ++;
				}
				p1 = g_utf8_next_char (p);
				remain -= p1 - p;
				p = p1;
			}
		}
	}

	return ((double)mark / (double)total) > chartable_module_ctx->threshold;
}

static int 
chartable_mime_filter (struct worker_task *task)
{	
	GList *cur;

	if (check_view (task->cfg->views, chartable_module_ctx->symbol, task)) {
		cur = g_list_first (task->text_parts);
		while (cur) {
			if (check_part ((struct mime_text_part *)cur->data, task->cfg->raw_mode)) {
				insert_result (task, chartable_module_ctx->metric, chartable_module_ctx->symbol, 1, NULL);	
			}
			cur = g_list_next (cur);
		}
	}

	return 0;
}

