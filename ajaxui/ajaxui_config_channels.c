/*
 *  tvheadend, AJAX / HTML user interface
 *  Copyright (C) 2008 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "tvhead.h"
#include "http.h"
#include "ajaxui.h"
#include "channels.h"



/**
 * Render a channel group widget
 */
static void
ajax_chgroup_build(htsbuf_queue_t *tq, channel_group_t *tcg)
{
  htsbuf_qprintf(tq, "<li id=\"chgrp_%d\">", tcg->tcg_tag);
  
  ajax_box_begin(tq, AJAX_BOX_BORDER, NULL, NULL, NULL);
  
  htsbuf_qprintf(tq, "<div style=\"overflow: auto; width: 100%\">");
  
  htsbuf_qprintf(tq,
	      "<div style=\"float: left; width: 60%\">"
	      "<a href=\"javascript:void(0)\" "
	      "onClick=\"$('cheditortab').innerHTML=''; "
	      "new Ajax.Updater('groupeditortab', "
	      "'/ajax/chgroup_editor/%d', "
	      "{method: 'get', evalScripts: true})\" >"
	      "%s</a></div>",
	      tcg->tcg_tag, tcg->tcg_name);


  if(tcg != defgroup) {
    htsbuf_qprintf(tq,
		"<div style=\"float: left; width: 40%\" "
		"class=\"chgroupaction\">"
		"<a href=\"javascript:void(0)\" "
		"onClick=\"dellistentry('/ajax/chgroup_del','%d', '%s');\""
		">Delete</a></div>",
		tcg->tcg_tag, tcg->tcg_name);
  }
  

  htsbuf_qprintf(tq, "</div>");
  ajax_box_end(tq, AJAX_BOX_BORDER);
  htsbuf_qprintf(tq, "</li>");
}

/**
 * Update order of channel groups
 */
static int
ajax_chgroup_updateorder(http_connection_t *hc, http_reply_t *hr, 
			 const char *remain, void *opaque)
{
  channel_group_t *tcg;
  htsbuf_queue_t *tq = &hr->hr_q;
  http_arg_t *ra;

  TAILQ_FOREACH(ra, &hc->hc_req_args, link) {
    if(strcmp(ra->key, "channelgrouplist[]") ||
       (tcg = channel_group_by_tag(atoi(ra->val))) == NULL)
      continue;
    
    TAILQ_REMOVE(&all_channel_groups, tcg, tcg_global_link);
    TAILQ_INSERT_TAIL(&all_channel_groups, tcg, tcg_global_link);
  }

  channel_group_settings_write();

  htsbuf_qprintf(tq, "<span id=\"updatedok\">Updated on server</span>");
  ajax_js(tq, "Effect.Fade('updatedok')");
  http_output_html(hc, hr);
  return 0;
}



/**
 * Add a new channel group
 */
static int
ajax_chgroup_add(http_connection_t *hc, http_reply_t *hr, 
		 const char *remain, void *opaque)
{
  channel_group_t *tcg;
  htsbuf_queue_t *tq = &hr->hr_q;
  const char *name;
  
  if((name = http_arg_get(&hc->hc_req_args, "name")) != NULL) {

    TAILQ_FOREACH(tcg, &all_channel_groups, tcg_global_link)
      if(!strcmp(name, tcg->tcg_name))
	break;

    if(tcg == NULL) {
      tcg = channel_group_find(name, 1);

      ajax_chgroup_build(tq, tcg);

      /* We must recreate the Sortable object */

      ajax_js(tq, "Sortable.destroy(\"channelgrouplist\")");

      ajax_js(tq, "Sortable.create(\"channelgrouplist\", "
	      "{onUpdate:function(){updatelistonserver("
	      "'channelgrouplist', "
	      "'/ajax/chgroup_updateorder', "
	      "'list-info'"
	      ")}});");
    }
  }
  http_output_html(hc, hr);
  return 0;
}



/**
 * Delete a channel group
 */
static int
ajax_chgroup_del(http_connection_t *hc, http_reply_t *hr, 
		 const char *remain, void *opaque)
{
  channel_group_t *tcg;
  htsbuf_queue_t *tq = &hr->hr_q;
  const char *id;
  
  if((id = http_arg_get(&hc->hc_req_args, "id")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  if((tcg = channel_group_by_tag(atoi(id))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  htsbuf_qprintf(tq, "$('chgrp_%d').remove();", tcg->tcg_tag);
  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);

  channel_group_destroy(tcg);
  return 0;
}



/**
 * Channel group & channel configuration
 */
int
ajax_config_channels_tab(http_connection_t *hc, http_reply_t *hr)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_group_t *tcg;

  htsbuf_qprintf(tq, "<div style=\"float: left; width: 30%\">");

  ajax_box_begin(tq, AJAX_BOX_SIDEBOX, "channelgroups",
		 NULL, "Channel groups");

  htsbuf_qprintf(tq, "<div style=\"height:15px; text-align:center\" "
	      "id=\"list-info\"></div>");
   
  htsbuf_qprintf(tq, "<ul id=\"channelgrouplist\" class=\"draglist\">");

  TAILQ_FOREACH(tcg, &all_channel_groups, tcg_global_link) {
    if(tcg->tcg_hidden)
      continue;
    ajax_chgroup_build(tq, tcg);
  }

  htsbuf_qprintf(tq, "</ul>");
 
  ajax_js(tq, "Sortable.create(\"channelgrouplist\", "
	  "{onUpdate:function(){updatelistonserver("
	  "'channelgrouplist', "
	  "'/ajax/chgroup_updateorder', "
	  "'list-info'"
	  ")}});");

  /**
   * Add new group
   */

  htsbuf_qprintf(tq, "<hr>");

  ajax_box_begin(tq, AJAX_BOX_BORDER, NULL, NULL, NULL);

  htsbuf_qprintf(tq,
	      "<div style=\"height: 25px\">"
	      "<div style=\"float: left\">"
	      "<input type=\"text\" id=\"newchgrp\">"
	      "</div>"
	      "<div style=\"float: right\">"
	      "<input type=\"button\" value=\"Add\" "
	      "onClick=\"javascript:addlistentry_by_widget("
	      "'channelgrouplist', 'chgroup_add', 'newchgrp');\">"
	      "</div></div>");
    
  ajax_box_end(tq, AJAX_BOX_BORDER);
    
  ajax_box_end(tq, AJAX_BOX_SIDEBOX);
  htsbuf_qprintf(tq, "</div>");

  htsbuf_qprintf(tq, 
	      "<div id=\"groupeditortab\" "
	      "style=\"overflow: auto; float: left; width: 30%\"></div>");

  htsbuf_qprintf(tq, 
	      "<div id=\"cheditortab\" "
	      "style=\"overflow: auto; float: left; width: 40%\"></div>");

  http_output_html(hc, hr);
  return 0;
}

/**
 * Display all channels within the group
 */
static int
ajax_chgroup_editor(http_connection_t *hc, http_reply_t *hr,
		    const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch;
  channel_group_t *tcg, *tcg2;
  th_transport_t *t;
  char buf[10];
  int nsources;
  ajax_table_t ta;

  if(remain == NULL || (tcg = channel_group_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  htsbuf_qprintf(tq, "<script type=\"text/javascript\">\r\n"
	      "//<![CDATA[\r\n");
  
  /* Select all */
  htsbuf_qprintf(tq, "select_all = function() {\r\n");
  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    htsbuf_qprintf(tq, 
		"$('sel_%d').checked = true;\r\n",
		ch->ch_tag);
  }
  htsbuf_qprintf(tq, "}\r\n");

  /* Select none */
  htsbuf_qprintf(tq, "select_none = function() {\r\n");
  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    htsbuf_qprintf(tq, 
		"$('sel_%d').checked = false;\r\n",
		ch->ch_tag);
  }
  htsbuf_qprintf(tq, "}\r\n");

  /* Invert selection */
  htsbuf_qprintf(tq, "select_invert = function() {\r\n");
  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    htsbuf_qprintf(tq, 
		"$('sel_%d').checked = !$('sel_%d').checked;\r\n",
		ch->ch_tag, ch->ch_tag);
  }
  htsbuf_qprintf(tq, "}\r\n");

  /* Invert selection */
  htsbuf_qprintf(tq, "select_sources = function() {\r\n");
  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    htsbuf_qprintf(tq, 
		"$('sel_%d').checked = %s;\r\n",
		ch->ch_tag, LIST_FIRST(&ch->ch_transports) ? "true" : "false");
  }
  htsbuf_qprintf(tq, "}\r\n");



  /* Invoke AJAX call containing all selected elements */
  htsbuf_qprintf(tq, 
	      "select_do = function(op, arg1, arg2, check) {\r\n"
	      "if(check == true && !confirm(\"Are you sure?\")) {return;}\r\n"
	      "var h = new Hash();\r\n"
	      "h.set('arg1', arg1);\r\n"
	      "h.set('arg2', arg2);\r\n"
	      );
  
  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    htsbuf_qprintf(tq, 
		"if($('sel_%d').checked) {h.set('%d', 'selected') }\r\n",
		ch->ch_tag, ch->ch_tag);
  }

  htsbuf_qprintf(tq, " new Ajax.Request('/ajax/chop/' + op, "
	      "{parameters: h});\r\n");
  htsbuf_qprintf(tq, "}\r\n");


  htsbuf_qprintf(tq, 
	      "\r\n//]]>\r\n"
	      "</script>\r\n");


  ajax_box_begin(tq, AJAX_BOX_SIDEBOX, NULL, NULL, tcg->tcg_name);

  ajax_table_top(&ta, hc, tq, (const char *[])
		 {"Channelname", "Sources", "", NULL},
		 (int[]){8,2,1});

  TAILQ_FOREACH(ch, &tcg->tcg_channels, ch_group_link) {
    snprintf(buf, sizeof(buf), "%d", ch->ch_tag);
    ajax_table_row_start(&ta, buf);

    nsources = 0;

    LIST_FOREACH(t, &ch->ch_transports, tht_ch_link)
      nsources++;

    ajax_table_cell(&ta, NULL,
		    "<a href=\"javascript:void(0)\" "
		    "onclick=\"new Ajax.Updater('cheditortab', "
		    "'/ajax/cheditor/%d', {method: 'get'})\""
		    ">%s</a>", ch->ch_tag, ch->ch_name);

    ajax_table_cell(&ta, NULL, "%d", nsources);
    ajax_table_cell_checkbox(&ta);
  }
  ajax_table_bottom(&ta);

  htsbuf_qprintf(tq, "<hr>\r\n");
  htsbuf_qprintf(tq, "<div style=\"text-align: center; "
	      "overflow: auto; width: 100%\">");

  ajax_button(tq, "Select all", "select_all()");
  ajax_button(tq, "Select none", "select_none()");
  ajax_button(tq, "Invert selection", "select_invert()");
  ajax_button(tq, "Select channels with sources", "select_sources()");
  htsbuf_qprintf(tq, "</div>\r\n");

  htsbuf_qprintf(tq, "<hr>\r\n");

  htsbuf_qprintf(tq, "<div style=\"text-align: center; "
	      "overflow: auto; width: 100%\">");
  
  ajax_button(tq,
	      "Delete all selected...", 
	      "select_do('delete', '%d', 0, true);", tcg->tcg_tag);
  
  htsbuf_qprintf(tq,
	      "<select id=\"movetarget\" "
	      "onChange=\"select_do('changegroup', "
	      "$('movetarget').value, '%d', false)\">", tcg->tcg_tag);
  htsbuf_qprintf(tq, 
	      "<option value="">Move selected channels to group:</option>");

  TAILQ_FOREACH(tcg2, &all_channel_groups, tcg_global_link) {
    if(tcg2->tcg_hidden || tcg == tcg2)
      continue;
    htsbuf_qprintf(tq, "<option value=\"%d\">%s</option>",
		tcg2->tcg_tag, tcg2->tcg_name);
  }
  htsbuf_qprintf(tq, "</select></div>");
  htsbuf_qprintf(tq, "</div>");
  htsbuf_qprintf(tq, "</div>");
  ajax_box_end(tq, AJAX_BOX_SIDEBOX);


  http_output_html(hc, hr);
  return 0;
}


/**
 *
 */
static struct strtab sourcetypetab[] = {
  { "DVB",        TRANSPORT_DVB },
  { "V4L",        TRANSPORT_V4L },
  { "IPTV",       TRANSPORT_IPTV },
  { "AVgen",      TRANSPORT_AVGEN },
  { "File",       TRANSPORT_STREAMEDFILE },
};


static struct strtab cdlongname[] = {
  { "None",                  COMMERCIAL_DETECT_NONE },
  { "Swedish TV4 Teletext",  COMMERCIAL_DETECT_TTP192 },
};

/**
 * Display all channels within the group
 */
static int
ajax_cheditor(http_connection_t *hc, http_reply_t *hr,
	      const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch, *ch2;
  channel_group_t *chg;
  th_transport_t *t;
  const char *s;
  int i;

  if(remain == NULL || (ch = channel_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  ajax_box_begin(tq, AJAX_BOX_SIDEBOX, NULL, NULL, ch->ch_name);
  
  if(ch->ch_icon != NULL) {
    htsbuf_qprintf(tq, 
		"<div style=\"width: 100%; text-align:center\">"
		"<img src=\"%s\"></div>", ch->ch_icon);
  }
  
  htsbuf_qprintf(tq, "<div>Sources:</div>");

  LIST_FOREACH(t, &ch->ch_transports, tht_ch_link) {
    ajax_box_begin(tq, AJAX_BOX_BORDER, NULL, NULL, NULL);
    htsbuf_qprintf(tq, "<div style=\"overflow: auto; width: 100%\">");
    htsbuf_qprintf(tq, "<div style=\"float: left; width: 13%%\">%s</div>",
		val2str(t->tht_type, sourcetypetab) ?: "???");
    htsbuf_qprintf(tq, "<div style=\"float: left; width: 87%%\">\"%s\"%s</div>",
		t->tht_svcname, t->tht_scrambled ? " - (scrambled)" : "");
    s = t->tht_sourcename ? t->tht_sourcename(t) : NULL;

    htsbuf_qprintf(tq, "</div><div style=\"overflow: auto; width: 100%\">");

    htsbuf_qprintf(tq,
		"<div style=\"float: left; width: 13%%\">"
		"<input %stype=\"checkbox\" class=\"nicebox\" "
		"onClick=\"new Ajax.Request('/ajax/transport_chdisable/%s', "
		"{parameters: {enabled: this.checked}});\">"
		"</div>", t->tht_disabled ? "" : "checked ",
		t->tht_identifier);

    if(s != NULL)
      htsbuf_qprintf(tq, "<div style=\"float: left; width: 87%%\">%s</div>",
		  s);

    htsbuf_qprintf(tq, "</div>");

    ajax_box_end(tq, AJAX_BOX_BORDER);
  }

  htsbuf_qprintf(tq, "<hr>\r\n");

  htsbuf_qprintf(tq, "<div style=\"overflow: auto; width:100%%\">");

  htsbuf_qprintf(tq, 
	      "<input type=\"button\" value=\"Rename...\" "
	      "onClick=\"channel_rename('%d', '%s')\">",
	      ch->ch_tag, ch->ch_name);

  htsbuf_qprintf(tq, 
	      "<input type=\"button\" value=\"Delete...\" "
	      "onClick=\"channel_delete('%d', '%s')\">",
	      ch->ch_tag, ch->ch_name);

  htsbuf_qprintf(tq,
	      "<select "
	      "onChange=\"channel_merge('%d', this.value);\">",
	      ch->ch_tag);
  
  htsbuf_qprintf(tq, "<option value=\"n\">Merge to channel:</option>");

  
  TAILQ_FOREACH(chg, &all_channel_groups, tcg_global_link) {
    TAILQ_FOREACH(ch2, &chg->tcg_channels, ch_group_link) {
      if(ch2 != ch)
	htsbuf_qprintf(tq, "<option value=\"%d\">%s</option>",
		    ch2->ch_tag, ch2->ch_name);
    }
  }
  
  htsbuf_qprintf(tq, "</select>");
  htsbuf_qprintf(tq, "</div>");
  htsbuf_qprintf(tq, "<hr>\r\n");

  htsbuf_qprintf(tq,
	      "<div class=\"infoprefixwidewidefat\">"
	      "Commercial detection:</div>"
	      "<div>"
	      "<select "
	      "onChange=\"new Ajax.Request('/ajax/chsetcomdetect/%d', "
	      "{parameters: {how: this.value}});\">",
	      ch->ch_tag);

  for(i = 0; i < sizeof(cdlongname) / sizeof(cdlongname[0]); i++) {
    htsbuf_qprintf(tq, "<option %svalue=%d>%s</option>",
		cdlongname[i].val == ch->ch_commercial_detection ? 
		"selected " : "",
		cdlongname[i].val, cdlongname[i].str);
  }
  htsbuf_qprintf(tq, "</select></div>");
  htsbuf_qprintf(tq, "</div>");


  ajax_box_end(tq, AJAX_BOX_SIDEBOX);
  http_output_html(hc, hr);
  return 0;
}

/**
 * Change group for channel(s)
 */
static int
ajax_changegroup(http_connection_t *hc, http_reply_t *hr,
		 const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch;
  channel_group_t *tcg;
  http_arg_t *ra;
  const char *s;
  const char *curgrp;

  if((s = http_arg_get(&hc->hc_req_args, "arg1")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  if((curgrp = http_arg_get(&hc->hc_req_args, "arg2")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  tcg = channel_group_by_tag(atoi(s));
  if(tcg == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  TAILQ_FOREACH(ra, &hc->hc_req_args, link) {
    if(strcmp(ra->val, "selected"))
      continue;
    
    if((ch = channel_by_tag(atoi(ra->key))) != NULL)
      channel_set_group(ch, tcg);
  }

  htsbuf_qprintf(tq,
	      "$('cheditortab').innerHTML=''; "
	      "new Ajax.Updater('groupeditortab', "
	      "'/ajax/chgroup_editor/%s', "
	      "{method: 'get', evalScripts: true});", curgrp);
  
  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}

/**
 * Change commercial detection type for channel(s)
 */
static int
ajax_chsetcomdetect(http_connection_t *hc, http_reply_t *hr,
		    const char *remain, void *opaque)
{
  channel_t *ch;
  const char *s;

  if(remain == NULL || (ch = channel_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;
  
  if((s = http_arg_get(&hc->hc_req_args, "how")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;
  
  ch->ch_commercial_detection = atoi(s);

  channel_settings_write(ch);
  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}


/**
 * Rename a channel
 */
static int
ajax_chrename(http_connection_t *hc, http_reply_t *hr,
	      const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch;
  const char *s;

  if(remain == NULL || (ch = channel_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;
  
  if((s = http_arg_get(&hc->hc_req_args, "newname")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  if(channel_rename(ch, s)) {
    htsbuf_qprintf(tq, "alert('Channel already exist');");
  } else {
    htsbuf_qprintf(tq, 
		"new Ajax.Updater('groupeditortab', "
		"'/ajax/chgroup_editor/%d', "
		"{method: 'get', evalScripts: true});\r\n",
		ch->ch_group->tcg_tag);
 
    htsbuf_qprintf(tq, 
		"new Ajax.Updater('cheditortab', "
		"'/ajax/cheditor/%d', "
		"{method: 'get', evalScripts: true});\r\n",
		ch->ch_tag);
  }

  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}


/**
 * Delete channel
 */
static int
ajax_chdelete(http_connection_t *hc, http_reply_t *hr,
	      const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch;
  channel_group_t *tcg;

  if(remain == NULL || (ch = channel_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;
  
  tcg = ch->ch_group;
  
  channel_delete(ch);

  htsbuf_qprintf(tq, 
	      "new Ajax.Updater('groupeditortab', "
	      "'/ajax/chgroup_editor/%d', "
	      "{method: 'get', evalScripts: true});\r\n",
	      tcg->tcg_tag);
 
  htsbuf_qprintf(tq, "$('cheditortab').innerHTML='';\r\n");

  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}

/**
 * Merge channel
 */
static int
ajax_chmerge(http_connection_t *hc, http_reply_t *hr,
	     const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *src, *dst;
  channel_group_t *tcg;
  const char *s;

  if(remain == NULL || (src = channel_by_tag(atoi(remain))) == NULL)
    return HTTP_STATUS_NOT_FOUND;
  
  if((s = http_arg_get(&hc->hc_req_args, "dst")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;
  
  if((dst = channel_by_tag(atoi(s))) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  tcg = src->ch_group;
  channel_merge(dst, src);

  htsbuf_qprintf(tq, 
	      "new Ajax.Updater('groupeditortab', "
	      "'/ajax/chgroup_editor/%d', "
	      "{method: 'get', evalScripts: true});\r\n",
	      tcg->tcg_tag);
 
  htsbuf_qprintf(tq, "$('cheditortab').innerHTML='';\r\n");

  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}

/**
 * Change group for channel(s)
 */
static int
ajax_chdeletemulti(http_connection_t *hc, http_reply_t *hr,
		   const char *remain, void *opaque)
{
  htsbuf_queue_t *tq = &hr->hr_q;
  channel_t *ch;
  http_arg_t *ra;
  const char *curgrp;

  if((curgrp = http_arg_get(&hc->hc_req_args, "arg1")) == NULL)
    return HTTP_STATUS_BAD_REQUEST;

  TAILQ_FOREACH(ra, &hc->hc_req_args, link) {
    if(strcmp(ra->val, "selected"))
      continue;
    
    if((ch = channel_by_tag(atoi(ra->key))) != NULL)
      channel_delete(ch);
  }

  htsbuf_qprintf(tq,
	      "$('cheditortab').innerHTML=''; "
	      "new Ajax.Updater('groupeditortab', "
	      "'/ajax/chgroup_editor/%s', "
	      "{method: 'get', evalScripts: true});", curgrp);
  
  http_output(hc, hr, "text/javascript; charset=UTF-8", NULL, 0);
  return 0;
}



/**
 *
 */
void
ajax_config_channels_init(void)
{
  http_path_add("/ajax/chgroup_add"        , NULL, ajax_chgroup_add,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chgroup_del"        , NULL, ajax_chgroup_del,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chgroup_updateorder", NULL, ajax_chgroup_updateorder,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chgroup_editor",      NULL, ajax_chgroup_editor,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/cheditor",            NULL, ajax_cheditor,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chop/changegroup",    NULL, ajax_changegroup,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chsetcomdetect",      NULL, ajax_chsetcomdetect,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chrename",            NULL, ajax_chrename,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chdelete",            NULL, ajax_chdelete,
		AJAX_ACCESS_CONFIG);
  http_path_add("/ajax/chmerge",             NULL, ajax_chmerge,
		AJAX_ACCESS_CONFIG);
 http_path_add("/ajax/chop/delete",          NULL, ajax_chdeletemulti,
		AJAX_ACCESS_CONFIG);

}
