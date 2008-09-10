/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <utils.h>
#include <crm/pengine/rules.h>
#include <crm/msg_xml.h>
#include <clplumbing/cl_misc.h>

extern xmlNode *get_object_root(const char *object_type,xmlNode *the_root);
void populate_hash(xmlNode *nvpair_list, GHashTable *hash,
		   const char **attrs, int attrs_length);

resource_object_functions_t resource_class_functions[] = {
	{
		native_unpack,
		native_find_child,
		native_children,
		native_parameter,
		native_print,
		native_active,
		native_resource_state,
		native_location,
		native_free
	},
	{
		group_unpack,
		native_find_child,
		native_children,
		native_parameter,
		group_print,
		group_active,
		group_resource_state,
		native_location,
		group_free
	},
	{
		clone_unpack,
		native_find_child,
		native_children,
		native_parameter,
		clone_print,
		clone_active,
		clone_resource_state,
		native_location,
		clone_free
	},
	{
		master_unpack,
		native_find_child,
		native_children,
		native_parameter,
		clone_print,
		clone_active,
		clone_resource_state,
		native_location,
		clone_free
	}
};

int get_resource_type(const char *name)
{
	if(safe_str_eq(name, XML_CIB_TAG_RESOURCE)) {
		return pe_native;

	} else if(safe_str_eq(name, XML_CIB_TAG_GROUP)) {
		return pe_group;

	} else if(safe_str_eq(name, XML_CIB_TAG_INCARNATION)) {
		return pe_clone;

	} else if(safe_str_eq(name, XML_CIB_TAG_MASTER)) {
		return pe_master;
	}
	
	return pe_unknown;
}

static void dup_attr(gpointer key, gpointer value, gpointer user_data)
{
	add_hash_param(user_data, key, value);
}

void
get_meta_attributes(GHashTable *meta_hash, resource_t *rsc,
		    node_t *node, pe_working_set_t *data_set)
{
	GHashTable *node_hash = NULL;
	xmlNode *defaults = get_object_root(XML_CIB_TAG_RSCCONFIG, data_set->input);
	if(node) {
		node_hash = node->details->attrs;
	}
	
	unpack_instance_attributes(defaults, XML_TAG_META_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);
	
	xml_prop_iter(rsc->xml, prop_name, prop_value,
		      add_hash_param(meta_hash, prop_name, prop_value);
		);

	unpack_instance_attributes(rsc->xml, XML_TAG_META_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);

	/* populate from the regular attributes until the GUI can create
	 * meta attributes
	 */
	unpack_instance_attributes(rsc->xml, XML_TAG_ATTR_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);

	/* set anything else based on the parent */
	if(rsc->parent != NULL) {
		g_hash_table_foreach(rsc->parent->meta, dup_attr, meta_hash);
	}	
}

gboolean	
common_unpack(xmlNode * xml_obj, resource_t **rsc,
	      resource_t *parent, pe_working_set_t *data_set)
{
	xmlNode *ops = NULL;
	const char *value = NULL;
	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *class = crm_element_value(xml_obj, XML_AGENT_ATTR_CLASS);

	crm_log_xml_debug_3(xml_obj, "Processing resource input...");
	
	if(id == NULL) {
		pe_err("Must specify id tag in <resource>");
		return FALSE;
		
	} else if(rsc == NULL) {
		pe_err("Nowhere to unpack resource into");
		return FALSE;
		
	}
	crm_malloc0(*rsc, sizeof(resource_t));
	ops = find_xml_node(xml_obj, "operations", FALSE);
	
	(*rsc)->xml  = xml_obj;
	(*rsc)->parent  = parent;
	(*rsc)->ops_xml = expand_idref(ops);

	(*rsc)->variant = get_resource_type(crm_element_name(xml_obj));
	if((*rsc)->variant == pe_unknown) {
		pe_err("Unknown resource type: %s", crm_element_name(xml_obj));
		crm_free(*rsc);
		return FALSE;
	}
	
	(*rsc)->parameters = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);
	
	(*rsc)->meta = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);
	
	value = crm_element_value(xml_obj, XML_RSC_ATTR_INCARNATION);
	if(value) {
		(*rsc)->id = crm_concat(id, value, ':');
		add_hash_param((*rsc)->meta, XML_RSC_ATTR_INCARNATION, value);
		
	} else {
		(*rsc)->id = crm_strdup(id);
	}

	if(parent) {
		(*rsc)->long_name = crm_concat(parent->long_name, (*rsc)->id, ':');
	} else {
		(*rsc)->long_name = crm_strdup((*rsc)->id);
	}
	
	(*rsc)->fns = &resource_class_functions[(*rsc)->variant];
	crm_debug_3("Unpacking resource...");

	get_meta_attributes((*rsc)->meta, *rsc, NULL, data_set);

	if(parent != NULL) {
		g_hash_table_foreach(
			parent->parameters, dup_attr, (*rsc)->parameters);
	}

	(*rsc)->flags = 0;
	set_bit((*rsc)->flags, pe_rsc_runnable); 
	set_bit((*rsc)->flags, pe_rsc_provisional); 

	if(is_set(data_set->flags, pe_flag_is_managed_default)) {
	    set_bit((*rsc)->flags, pe_rsc_managed); 
	}

	(*rsc)->rsc_cons	   = NULL; 
	(*rsc)->actions            = NULL;
	(*rsc)->role		   = RSC_ROLE_STOPPED;
	(*rsc)->next_role	   = RSC_ROLE_UNKNOWN;

	(*rsc)->recovery_type      = recovery_stop_start;
	(*rsc)->stickiness         = data_set->default_resource_stickiness;
	(*rsc)->migration_threshold= data_set->default_migration_threshold;
	(*rsc)->failure_timeout    = data_set->default_failure_timeout;

	value = g_hash_table_lookup((*rsc)->meta, XML_CIB_ATTR_PRIORITY);
	(*rsc)->priority	   = crm_parse_int(value, "0"); 
	(*rsc)->effective_priority = (*rsc)->priority;

	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_NOTIFY);
	if(crm_is_true(value)) {
	    set_bit((*rsc)->flags, pe_rsc_notify); 
	}
	
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_MANAGED);
	if(value != NULL && safe_str_neq("default", value)) {
	    gboolean bool_value = TRUE;
	    cl_str_to_boolean(value, &bool_value);
	    if(bool_value == FALSE) {
		clear_bit((*rsc)->flags, pe_rsc_managed); 
	    } else {
		set_bit((*rsc)->flags, pe_rsc_managed); 
	    }
	}

	if(is_set(data_set->flags, pe_flag_maintenance_mode)) {
	    clear_bit((*rsc)->flags, pe_rsc_managed);
	}
	
	crm_debug_2("Options for %s", (*rsc)->id);
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_UNIQUE);
	if(value == NULL || crm_is_true(value)) {
	    set_bit((*rsc)->flags, pe_rsc_unique); 
	}
	
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_RESTART);
	if(safe_str_eq(value, "restart")) {
		(*rsc)->restart_type = pe_restart_restart;
		crm_debug_2("\tDependancy restart handling: restart");

	} else {
		(*rsc)->restart_type = pe_restart_ignore;
		crm_debug_2("\tDependancy restart handling: ignore");
	}

	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_MULTIPLE);
	if(safe_str_eq(value, "stop_only")) {
		(*rsc)->recovery_type = recovery_stop_only;
		crm_debug_2("\tMultiple running resource recovery: stop only");

	} else if(safe_str_eq(value, "block")) {
		(*rsc)->recovery_type = recovery_block;
		crm_debug_2("\tMultiple running resource recovery: block");

	} else {		
		(*rsc)->recovery_type = recovery_stop_start;
		crm_debug_2("\tMultiple running resource recovery: stop/start");
	}

	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_STICKINESS);
	if(value != NULL && safe_str_neq("default", value)) {
		(*rsc)->stickiness = char2score(value);
	}

	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_FAIL_STICKINESS);
	if(value != NULL && safe_str_neq("default", value)) {
		(*rsc)->migration_threshold = char2score(value);
	}
	
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_FAIL_TIMEOUT);
	if(value != NULL) {
	    /* call crm_get_msec() and convert back to seconds */
	    (*rsc)->failure_timeout = (crm_get_msec(value) / 1000);
	}
	
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_TARGET_ROLE);
	if(is_set(data_set->flags, pe_flag_stop_everything)) {
	    (*rsc)->next_role = RSC_ROLE_STOPPED;

	} else if(value != NULL && safe_str_neq("default", value)) {
		(*rsc)->next_role = text2role(value);
		if((*rsc)->next_role == RSC_ROLE_UNKNOWN) {
			crm_config_err("%s: Unknown value for "
				       XML_RSC_ATTR_TARGET_ROLE": %s",
				       (*rsc)->id, value);
		}
	}
	

	crm_debug_2("\tDesired next state: %s",
		    (*rsc)->next_role!=RSC_ROLE_UNKNOWN?role2text((*rsc)->next_role):"default");

	if((*rsc)->fns->unpack(*rsc, data_set) == FALSE) {
		return FALSE;
	}
	
	if(is_set(data_set->flags, pe_flag_symmetric_cluster)) {
		resource_location(*rsc, NULL, 0, "symmetric_default", data_set);
	}
	
	crm_debug_2("\tAction notification: %s",
		    is_set((*rsc)->flags, pe_rsc_notify)?"required":"not required");

	if(safe_str_eq(class, "stonith")) {
	    set_bit_inplace(data_set->flags, pe_flag_have_stonith_resource);
	}
	
/* 	data_set->resources = g_list_append(data_set->resources, (*rsc)); */
	return TRUE;
}


void common_update_score(resource_t *rsc, const char *id, int score) 
{
    node_t *node = NULL;
    node = pe_find_node_id(rsc->allowed_nodes, id);
    if(node != NULL) {
	crm_debug_2("Updating score for %s on %s: %d + %d",
		    rsc->id, id, node->weight, score);
	node->weight = merge_weights(node->weight, score);
    }

    if(rsc->children) {
	slist_iter(
	    child_rsc, resource_t, rsc->children, lpc,
	    common_update_score(child_rsc, id, score);
	    );
    }
}

resource_t *uber_parent(resource_t *rsc) 
{
	resource_t *parent = rsc;
	while(parent != NULL && parent->parent != NULL) {
		parent = parent->parent;
	}
	return parent;
}

void common_free(resource_t *rsc)
{
	if(rsc == NULL) {
		return;
	}
	
	crm_debug_5("Freeing %s %d", rsc->id, rsc->variant);

	g_list_free(rsc->rsc_cons);
	g_list_free(rsc->rsc_cons_lhs);

	if(rsc->parameters != NULL) {
		g_hash_table_destroy(rsc->parameters);
	}
	if(rsc->meta != NULL) {
		g_hash_table_destroy(rsc->meta);
	}
	if(rsc->parent == NULL && is_set(rsc->flags, pe_rsc_orphan)) {
		free_xml(rsc->xml);
	}
	if(rsc->running_on) {
		g_list_free(rsc->running_on);
		rsc->running_on = NULL;
	}
	if(rsc->known_on) {
		g_list_free(rsc->known_on);
		rsc->known_on = NULL;
	}
	if(rsc->actions) {
		g_list_free(rsc->actions);
		rsc->actions = NULL;
	}
	pe_free_shallow_adv(rsc->rsc_location, FALSE);
	pe_free_shallow_adv(rsc->allowed_nodes, TRUE);
	crm_free(rsc->id);
	crm_free(rsc->long_name);	
	crm_free(rsc->clone_name);
	crm_free(rsc->allocated_to);
	crm_free(rsc->variant_opaque);
	crm_free(rsc);
	crm_debug_5("Resource freed");
}

