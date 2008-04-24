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

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <heartbeat.h>
#include <clplumbing/cl_log.h>

#include <time.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/msg.h>
#include <crm/common/xml.h>

enum cib_errors 
cib_process_query(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	xmlNode *obj_root = NULL;
	enum cib_errors result = cib_ok;

	crm_debug_2("Processing \"%s\" event for section=%s",
		  op, crm_str(section));

	CRM_CHECK(*answer == NULL, free_xml(*answer));
	*answer = NULL;
	
	if (safe_str_eq(XML_CIB_TAG_SECTION_ALL, section)) {
		section = NULL;
	}

	obj_root = get_object_root(section, existing_cib);
	
	if(obj_root == NULL) {
		result = cib_NOTEXISTS;

	} else {
		*answer = obj_root;
	}

	if(result == cib_ok && *answer == NULL) {
		crm_err("Error creating query response");
		result = cib_output_data;
	}
	
	return result;
}

enum cib_errors 
cib_process_erase(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	enum cib_errors result = cib_ok;

	crm_debug_2("Processing \"%s\" event", op);
	*answer = NULL;
	free_xml(*result_cib);
	*result_cib = createEmptyCib();

	copy_in_properties(*result_cib, existing_cib);	
	cib_update_counter(*result_cib, XML_ATTR_GENERATION, FALSE);
	
	return result;
}

enum cib_errors 
cib_process_bump(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	enum cib_errors result = cib_ok;

	crm_debug_2("Processing \"%s\" event for epoch=%s",
		  op, crm_str(crm_element_value(existing_cib, XML_ATTR_GENERATION)));
	
	*answer = NULL;
	cib_update_counter(*result_cib, XML_ATTR_GENERATION, FALSE);
	
	return result;
}


enum cib_errors 
cib_update_counter(xmlNode *xml_obj, const char *field, gboolean reset)
{
	char *new_value = NULL;
	char *old_value = NULL;
	int  int_value  = -1;
	
	if(reset == FALSE && crm_element_value(xml_obj, field) != NULL) {
		old_value = crm_element_value_copy(xml_obj, field);
	}
	if(old_value != NULL) {
		crm_malloc0(new_value, 128);
		int_value = atoi(old_value);
		sprintf(new_value, "%d", ++int_value);
	} else {
		new_value = crm_strdup("1");
	}

	crm_debug_4("%s %d(%s)->%s",
		  field, int_value, crm_str(old_value), crm_str(new_value));
	crm_xml_add(xml_obj, field, new_value);

	crm_free(new_value);
	crm_free(old_value);

	return cib_ok;
}

enum cib_errors 
cib_process_replace(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	const char *tag = NULL;
	gboolean verbose       = FALSE;
	enum cib_errors result = cib_ok;
	
	crm_debug_2("Processing \"%s\" event for section=%s",
		    op, crm_str(section));
	*answer = NULL;

	if (input == NULL) {
		return cib_NOOBJECT;
	}

	tag = crm_element_name(input);

	if (options & cib_verbose) {
		verbose = TRUE;
	}
	if(safe_str_eq(XML_CIB_TAG_SECTION_ALL, section)) {
		section = NULL;

	} else if(safe_str_eq(tag, section)) {
		section = NULL;
	}
	
	if(safe_str_eq(tag, XML_TAG_CIB)) {
		int updates = 0;
		int epoch  = 0;
		int admin_epoch = 0;
		
		int replace_updates = 0;
		int replace_epoch  = 0;
		int replace_admin_epoch = 0;
		const char *reason = NULL;
		
		cib_version_details(
			existing_cib, &admin_epoch, &epoch, &updates);
		cib_version_details(input, &replace_admin_epoch,
				    &replace_epoch, &replace_updates);

		if(replace_admin_epoch < admin_epoch) {
			reason = XML_ATTR_GENERATION_ADMIN;

		} else if(replace_admin_epoch > admin_epoch) {
			/* no more checks */

		} else if(replace_epoch < epoch) {
			reason = XML_ATTR_GENERATION;

		} else if(replace_epoch > epoch) {
			/* no more checks */

		} else if(replace_updates < updates) {
			reason = XML_ATTR_NUMUPDATES;
		}

		if(reason != NULL) {
			crm_warn("Replacement %d.%d.%d not applied to %d.%d.%d:"
				 " current %s is greater than the replacement",
				 replace_admin_epoch, replace_epoch,
				 replace_updates, admin_epoch, epoch, updates,
				 reason);
			result = cib_old_data;
		}

		/* sync_in_progress = 0; */
		crm_err("Set sync_in_progress=0");
		
		free_xml(*result_cib);
		*result_cib = copy_xml(input);
		
	} else {
		xmlNode *obj_root = NULL;
		gboolean ok = TRUE;
		obj_root = get_object_root(section, *result_cib);
		ok = replace_xml_child(NULL, obj_root, input, FALSE);
		if(ok == FALSE) {
			crm_debug_2("No matching object to replace");
			result = cib_NOTEXISTS;
		}
	}

	return result;
}

enum cib_errors 
cib_process_delete(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	xmlNode *obj_root = NULL;
	crm_debug_2("Processing \"%s\" event", op);

	if(input == NULL) {
		crm_err("Cannot perform modification with no data");
		return cib_NOOBJECT;
	}
	
	obj_root = get_object_root(section, *result_cib);
	
	crm_validate_data(input);
	crm_validate_data(*result_cib);

	if(replace_xml_child(NULL, obj_root, input, TRUE) == FALSE) {
		crm_debug_2("No matching object to delete");
	}
	
	return cib_ok;
}

enum cib_errors 
cib_process_modify(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	xmlNode *obj_root = NULL;
	crm_debug_2("Processing \"%s\" event", op);

	if(input == NULL) {
		crm_err("Cannot perform modification with no data");
		return cib_NOOBJECT;
	}
	
	obj_root = get_object_root(section, *result_cib);
	
	crm_validate_data(input);
	crm_validate_data(*result_cib);

	if(update_xml_child(obj_root, input) == FALSE) {
		return cib_NOTEXISTS;		
	}
	
	return cib_ok;
}

enum cib_errors 
cib_process_diff(
	const char *op, int options, const char *section, xmlNode *req, xmlNode *input,
	xmlNode *existing_cib, xmlNode **result_cib, xmlNode **answer)
{
	unsigned int log_level = LOG_DEBUG;
	const char *value = NULL;
	const char *reason = NULL;
	gboolean apply_diff = TRUE;
	enum cib_errors result = cib_ok;

	int this_updates = 0;
	int this_epoch  = 0;
	int this_admin_epoch = 0;

	int diff_add_updates = 0;
	int diff_add_epoch  = 0;
	int diff_add_admin_epoch = 0;

	int diff_del_updates = 0;
	int diff_del_epoch  = 0;
	int diff_del_admin_epoch = 0;

	crm_debug_2("Processing \"%s\" event", op);

	cib_diff_version_details(
		input,
		&diff_add_admin_epoch, &diff_add_epoch, &diff_add_updates, 
		&diff_del_admin_epoch, &diff_del_epoch, &diff_del_updates);

	
	value = crm_element_value(existing_cib, XML_ATTR_GENERATION);
	this_epoch = atoi(value?value:"0");
	
	value = crm_element_value(existing_cib, XML_ATTR_NUMUPDATES);
	this_updates = atoi(value?value:"0");
	
	value = crm_element_value(existing_cib, XML_ATTR_GENERATION_ADMIN);
	this_admin_epoch = atoi(value?value:"0");
	
	if(diff_del_admin_epoch == diff_add_admin_epoch
	   && diff_del_epoch == diff_add_epoch
	   && diff_del_updates == diff_add_updates) {
		if(diff_add_admin_epoch == -1 && diff_add_epoch == -1 && diff_add_updates == -1) {
			diff_add_epoch = this_epoch;
			diff_add_updates = this_updates + 1;
			diff_add_admin_epoch = this_admin_epoch;
			diff_del_epoch = this_epoch;
			diff_del_updates = this_updates;
			diff_del_admin_epoch = this_admin_epoch;
		} else {
			apply_diff = FALSE;
			log_level = LOG_ERR;
			reason = "+ and - versions in the diff did not change";
			log_cib_diff(LOG_ERR, input, __FUNCTION__);
		}
	}

	if(apply_diff && diff_del_admin_epoch > this_admin_epoch) {
		result = cib_diff_resync;
		apply_diff = FALSE;
		log_level = LOG_INFO;
		reason = "current \""XML_ATTR_GENERATION_ADMIN"\" is less than required";
		
	} else if(apply_diff && diff_del_admin_epoch < this_admin_epoch) {
		apply_diff = FALSE;
		log_level = LOG_WARNING;
		reason = "current \""XML_ATTR_GENERATION_ADMIN"\" is greater than required";
	}

	if(apply_diff && diff_del_epoch > this_epoch) {
		result = cib_diff_resync;
		apply_diff = FALSE;
		log_level = LOG_INFO;
		reason = "current \""XML_ATTR_GENERATION"\" is less than required";
		
	} else if(apply_diff && diff_del_epoch < this_epoch) {
		apply_diff = FALSE;
		log_level = LOG_WARNING;
		reason = "current \""XML_ATTR_GENERATION"\" is greater than required";
	}

	if(apply_diff && diff_del_updates > this_updates) {
		result = cib_diff_resync;
		apply_diff = FALSE;
		log_level = LOG_INFO;
		reason = "current \""XML_ATTR_NUMUPDATES"\" is less than required";
		
	} else if(apply_diff && diff_del_updates < this_updates) {
		apply_diff = FALSE;
		log_level = LOG_WARNING;
		reason = "current \""XML_ATTR_NUMUPDATES"\" is greater than required";
	}

	if(apply_diff) {
		free_xml(*result_cib);
		*result_cib = NULL;
		if(apply_xml_diff(existing_cib, input, result_cib) == FALSE) {
		    log_level = LOG_NOTICE;
		    reason = "Failed application of an update diff";
		    
		    if(options & cib_force_diff) {
			result = cib_diff_resync;
		    }
			
		} else if((options & cib_force_diff) && !validate_with_dtd(
			      *result_cib, FALSE, DTD_DIRECTORY"/crm.dtd")) {

		    log_level = LOG_NOTICE;
		    result = cib_diff_resync;
		    reason = "Failed DTD validation of a global update.";
		}
	}
	
	if(reason != NULL) {
		do_crm_log(
			log_level,
			"Diff %d.%d.%d -> %d.%d.%d not applied to %d.%d.%d: %s",
			diff_del_admin_epoch,diff_del_epoch,diff_del_updates,
			diff_add_admin_epoch,diff_add_epoch,diff_add_updates,
			this_admin_epoch,this_epoch,this_updates, reason);

		if(result == cib_ok) {
		    result = cib_diff_failed;
		}
		
	} else if(apply_diff) {
		crm_debug_2("Diff %d.%d.%d -> %d.%d.%d was applied",
			    diff_del_admin_epoch,diff_del_epoch,diff_del_updates,
			    diff_add_admin_epoch,diff_add_epoch,diff_add_updates);
	}
	return result;
}

gboolean
apply_cib_diff(xmlNode *old, xmlNode *diff, xmlNode **new)
{
	gboolean result = TRUE;
	const char *value = NULL;

	int this_updates = 0;
	int this_epoch  = 0;
	int this_admin_epoch = 0;

	int diff_add_updates = 0;
	int diff_add_epoch  = 0;
	int diff_add_admin_epoch = 0;

	int diff_del_updates = 0;
	int diff_del_epoch  = 0;
	int diff_del_admin_epoch = 0;

	CRM_CHECK(diff != NULL, return FALSE);
	CRM_CHECK(old != NULL, return FALSE);
	
	value = crm_element_value(old, XML_ATTR_GENERATION_ADMIN);
	this_admin_epoch = crm_parse_int(value, "0");
	crm_debug_3("%s=%d (%s)", XML_ATTR_GENERATION_ADMIN,
		  this_admin_epoch, value);
	
	value = crm_element_value(old, XML_ATTR_GENERATION);
	this_epoch = crm_parse_int(value, "0");
	crm_debug_3("%s=%d (%s)", XML_ATTR_GENERATION, this_epoch, value);
	
	value = crm_element_value(old, XML_ATTR_NUMUPDATES);
	this_updates = crm_parse_int(value, "0");
	crm_debug_3("%s=%d (%s)", XML_ATTR_NUMUPDATES, this_updates, value);
	
	cib_diff_version_details(
		diff,
		&diff_add_admin_epoch, &diff_add_epoch, &diff_add_updates, 
		&diff_del_admin_epoch, &diff_del_epoch, &diff_del_updates);

	value = NULL;
	if(result && diff_del_admin_epoch != this_admin_epoch) {
		value = XML_ATTR_GENERATION_ADMIN;
		result = FALSE;
		crm_debug_3("%s=%d", value, diff_del_admin_epoch);

	} else if(result && diff_del_epoch != this_epoch) {
		value = XML_ATTR_GENERATION;
		result = FALSE;
		crm_debug_3("%s=%d", value, diff_del_epoch);

	} else if(result && diff_del_updates != this_updates) {
		value = XML_ATTR_NUMUPDATES;
		result = FALSE;
		crm_debug_3("%s=%d", value, diff_del_updates);
	}

	if(result) {
		xmlNode *tmp = NULL;
		xmlNode *diff_copy = copy_xml(diff);
		
		tmp = find_xml_node(diff_copy, "diff-removed", TRUE);
		if(tmp != NULL) {
			xml_remove_prop(tmp, XML_ATTR_GENERATION_ADMIN);
			xml_remove_prop(tmp, XML_ATTR_GENERATION);
			xml_remove_prop(tmp, XML_ATTR_NUMUPDATES);
		}
		
		tmp = find_xml_node(diff_copy, "diff-added", TRUE);
		if(tmp != NULL) {
			xml_remove_prop(tmp, XML_ATTR_GENERATION_ADMIN);
			xml_remove_prop(tmp, XML_ATTR_GENERATION);
			xml_remove_prop(tmp, XML_ATTR_NUMUPDATES);
		}
		
		result = apply_xml_diff(old, diff_copy, new);
		free_xml(diff_copy);
		
	} else {
		crm_err("target and diff %s values didnt match", value);
	}
	
	
	return result;
}

gboolean
cib_config_changed(xmlNode *old_cib, xmlNode *new_cib, xmlNode **result)
{
	gboolean config_changes = FALSE;
	const char *tag = NULL;
	xmlNode *diff = NULL;
	xmlNode *dest = NULL;

	if(result) {
		*result = NULL;
	}

	diff = diff_xml_object(old_cib, new_cib, FALSE);
	if(diff == NULL) {
		return FALSE;
	}

	tag = "diff-removed";
	dest = find_xml_node(diff, tag, FALSE);
	if(dest) {
		dest = find_xml_node(dest, "cib", FALSE);
	}

	if(dest) {
		if(first_named_child(dest, "status")) {
		    xmlNode *status = first_named_child(dest, "status");
		    free_xml(status);
		}
		if(xml_has_children(dest)) {
			config_changes = TRUE;
		}
	}

	tag = "diff-added";
	dest = find_xml_node(diff, tag, FALSE);
	if(dest) {
		dest = find_xml_node(dest, "cib", FALSE);
	}

	if(dest) {
		if(first_named_child(dest, "status")) {
		    xmlNode *status = first_named_child(dest, "status");
		    free_xml(status);
		}

		xml_prop_iter(dest, name, value, config_changes = TRUE);
		
		if(xml_has_children(dest)) {
			config_changes = TRUE;
		}
	}

	
	if(result) {
		*result = diff;
	} else {
		free_xml(diff);
	}
	
	return config_changes;
}

xmlNode *
diff_cib_object(xmlNode *old_cib, xmlNode *new_cib, gboolean suppress)
{
	xmlNode *dest = NULL;
	xmlNode *src = NULL;
	const char *name = NULL;
	const char *value = NULL;

	xmlNode *diff = diff_xml_object(old_cib, new_cib, suppress);
	
	/* add complete version information */
	src = old_cib;
	dest = find_xml_node(diff, "diff-removed", FALSE);
	if(src != NULL && dest != NULL) {
		name = XML_ATTR_GENERATION_ADMIN;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);

		name = XML_ATTR_GENERATION;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);

		name = XML_ATTR_NUMUPDATES;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);
	}
	
	src = new_cib;
	dest = find_xml_node(diff, "diff-added", FALSE);
	if(src != NULL && dest != NULL) {
		name = XML_ATTR_GENERATION_ADMIN;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);

		name = XML_ATTR_GENERATION;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);

		name = XML_ATTR_NUMUPDATES;
		value = crm_element_value(src, name);
		if(value == NULL) {
			value = "0";
		}
		crm_xml_add(dest, name, value);
	}
	return diff;
}
