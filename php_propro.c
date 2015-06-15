/*
    +--------------------------------------------------------------------+
    | PECL :: propro                                                     |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013 Michael Wallner <mike@php.net>                  |
    +--------------------------------------------------------------------+
*/


#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>

#include "php_propro.h"

#define DEBUG_PROPRO 0

static inline zval *get_referenced_zval(zval *ref)
{
	while (Z_ISREF_P(ref)) {
		ref = Z_REFVAL_P(ref);
	}
	return ref;
}

php_property_proxy_t *php_property_proxy_init(zval *container, zend_string *member)
{
	php_property_proxy_t *proxy = ecalloc(1, sizeof(*proxy));

	ZVAL_COPY(&proxy->container, get_referenced_zval(container));
	proxy->member = zend_string_copy(member);

	return proxy;
}

void php_property_proxy_free(php_property_proxy_t **proxy)
{
	if (*proxy) {
		zval_ptr_dtor(&(*proxy)->container);
		zend_string_release((*proxy)->member);
		efree(*proxy);
		*proxy = NULL;
	}
}

static zend_class_entry *php_property_proxy_class_entry;
static zend_object_handlers php_property_proxy_object_handlers;

zend_class_entry *php_property_proxy_get_class_entry(void)
{
	return php_property_proxy_class_entry;
}

static inline php_property_proxy_object_t *get_propro(zval *object);
static zval *get_parent_proxied_value(zval *object, zval *return_value);
static zval *get_proxied_value(zval *object, zval *return_value);
static zval *read_dimension(zval *object, zval *offset, int type, zval *return_value);
static ZEND_RESULT_CODE cast_proxied_value(zval *object, zval *return_value, int type);
static void write_dimension(zval *object, zval *offset, zval *value);
static void set_proxied_value(zval *object, zval *value);

#if DEBUG_PROPRO
/* we do not really care about TS when debugging */
static int level = 1;
static const char space[] = "                               ";
static const char *inoutstr[] = {"< return","="," > enter"};

static void _walk(php_property_proxy_object_t *obj)
{
	if (obj) {
		if (!Z_ISUNDEF(obj->parent)) {
			_walk(get_propro(&obj->parent));
		}
		if (obj->proxy) {
			fprintf(stderr, ".%s", obj->proxy->member->val);
		}
	}
}

static void debug_propro(int inout, const char *f,
		php_property_proxy_object_t *obj, zval *offset, zval *value TSRMLS_DC)
{
	fprintf(stderr, "#PP %p %s %s %s ", obj, &space[sizeof(space)-level],
			inoutstr[inout+1], f);

	level += inout;

	_walk(obj);

	if (*f++=='d'
	&&	*f++=='i'
	&&	*f++=='m'
	) {
		char *offset_str = "[]";
		zval *o = offset;

		if (o) {
			convert_to_string_ex(o);
			offset_str = Z_STRVAL_P(o);
		}

		fprintf(stderr, ".%s", offset_str);

		if (o && o != offset) {
			zval_ptr_dtor(o);
		}
	}
	if (value && !Z_ISUNDEF_P(value)) {
		const char *t[] = {
				"UNDEF",
				"NULL",
				"FALSE",
				"TRUE",
				"int",
				"float",
				"string",
				"Array",
				"Object",
				"resource",
				"reference",
				"constant",
				"constant AST",
				"_BOOL",
				"callable",
				"indirect",
				"---",
				"pointer"
		};
		fprintf(stderr, " = (%s) ", t[Z_TYPE_P(value)&0xf]);
		if (!Z_ISUNDEF_P(value) && Z_TYPE_P(value) != IS_INDIRECT) {
			zend_print_flat_zval_r(value TSRMLS_CC);
		}
	}

	fprintf(stderr, "\n");
}
#else
#define debug_propro(l, f, obj, off, val)
#endif

php_property_proxy_object_t *php_property_proxy_object_new_ex(
		zend_class_entry *ce, php_property_proxy_t *proxy)
{
	php_property_proxy_object_t *o;

	if (!ce) {
		ce = php_property_proxy_class_entry;
	}

	o = ecalloc(1, sizeof(*o) + sizeof(zval) * (ce->default_properties_count - 1));
	zend_object_std_init(&o->zo, ce);
	object_properties_init(&o->zo, ce);

	o->proxy = proxy;
	o->zo.handlers = &php_property_proxy_object_handlers;

	debug_propro(0, "init", o, NULL, NULL);

	return o;
}

zend_object *php_property_proxy_object_new(zend_class_entry *ce)
{
	return &php_property_proxy_object_new_ex(ce, NULL)->zo;
}

static void destroy_obj(zend_object *object)
{
	php_property_proxy_object_t *o = PHP_PROPRO_PTR(object);

	debug_propro(0, "dtor", o, NULL, NULL);

	if (o->proxy) {
		php_property_proxy_free(&o->proxy);
	}
	if (!Z_ISUNDEF(o->parent)) {
		zval_ptr_dtor(&o->parent);
		ZVAL_UNDEF(&o->parent);
	}
	zend_object_std_dtor(object);
}

static inline php_property_proxy_object_t *get_propro(zval *object)
{
	object = get_referenced_zval(object);
	switch (Z_TYPE_P(object)) {
	case IS_OBJECT:
		break;

	EMPTY_SWITCH_DEFAULT_CASE();
	}
	return PHP_PROPRO_PTR(Z_OBJ_P(object));
}

static inline zend_bool got_value(zval *container, zval *value)
{
	zval identical;

	if (!Z_ISUNDEF_P(value)) {
		if (SUCCESS == is_identical_function(&identical, value, container)) {
			if (Z_TYPE(identical) != IS_TRUE) {
				return 1;
			}
		}
	}

	return 0;
}

static zval *get_parent_proxied_value(zval *object, zval *return_value)
{
	php_property_proxy_object_t *obj;

	obj = get_propro(object);
	debug_propro(1, "parent_get", obj, NULL, NULL);

	if (obj->proxy) {
		if (!Z_ISUNDEF(obj->parent)) {
			get_proxied_value(&obj->parent, return_value);
		}
	}

	debug_propro(-1, "parent_get", obj, NULL, return_value);

	return return_value;
}

static zval *get_proxied_value(zval *object, zval *return_value)
{
	zval *hash_value, *ref, prop_tmp;
	php_property_proxy_object_t *obj;

	obj = get_propro(object);
	debug_propro(1, "get", obj, NULL, NULL);

	if (obj->proxy) {
		if (!Z_ISUNDEF(obj->parent)) {
			zval parent_value;

			ZVAL_UNDEF(&parent_value);
			get_parent_proxied_value(object, &parent_value);

			if (got_value(&obj->proxy->container, &parent_value)) {
				zval_ptr_dtor(&obj->proxy->container);
				ZVAL_COPY(&obj->proxy->container, &parent_value);
			}
		}

		ref = get_referenced_zval(&obj->proxy->container);

		switch (Z_TYPE_P(ref)) {
		case IS_OBJECT:
			RETVAL_ZVAL(zend_read_property(Z_OBJCE_P(ref), ref,
					obj->proxy->member->val, obj->proxy->member->len, 0, &prop_tmp),
					0, 0);
			break;

		case IS_ARRAY:
			hash_value = zend_symtable_find(Z_ARRVAL_P(ref), obj->proxy->member);

			if (hash_value) {
				RETVAL_ZVAL(hash_value, 0, 0);
			}
			break;
		}
	}

	debug_propro(-1, "get", obj, NULL, return_value);

	return return_value;
}

static ZEND_RESULT_CODE cast_proxied_value(zval *object, zval *return_value,
		int type)
{
	get_proxied_value(object, return_value);

	debug_propro(0, "cast", get_propro(object), NULL, return_value);

	if (!Z_ISUNDEF_P(return_value)) {
		convert_to_explicit_type_ex(return_value, type);
		return SUCCESS;
	}

	return FAILURE;
}

static void set_proxied_value(zval *object, zval *value)
{
	php_property_proxy_object_t *obj;
	zval *ref;

	obj = get_propro(object);
	debug_propro(1, "set", obj, NULL, value TSRMLS_CC);

	if (obj->proxy) {
		if (!Z_ISUNDEF(obj->parent)) {
			zval parent_value;

			ZVAL_UNDEF(&parent_value);
			get_parent_proxied_value(object, &parent_value);

			if (got_value(&obj->proxy->container, &parent_value)) {
				zval_ptr_dtor(&obj->proxy->container);
				ZVAL_COPY(&obj->proxy->container, &parent_value);
			}
		}

		ref = get_referenced_zval(&obj->proxy->container);

		switch (Z_TYPE_P(ref)) {
		case IS_OBJECT:
			zend_update_property(Z_OBJCE_P(ref), ref, obj->proxy->member->val,
					obj->proxy->member->len, value);
			break;

		default:
			convert_to_array(ref);
			/* no break */

		case IS_ARRAY:
			Z_TRY_ADDREF_P(value);
			zend_symtable_update(Z_ARRVAL_P(ref), obj->proxy->member, value);
			break;
		}

		if (!Z_ISUNDEF(obj->parent)) {
			set_proxied_value(&obj->parent, &obj->proxy->container);
		}
	}

	debug_propro(-1, "set", obj, NULL, NULL);
}

static zval *read_dimension(zval *object, zval *offset, int type, zval *return_value)
{
	zval proxied_value;
	zend_string *member = offset ? zval_get_string(offset) : NULL;

	debug_propro(1, type == BP_VAR_R ? "dim_read" : "dim_read_ref",
			get_propro(object), offset, NULL);

	ZVAL_UNDEF(&proxied_value);
	get_proxied_value(object, &proxied_value);

	if (BP_VAR_R == type && member && !Z_ISUNDEF(proxied_value)) {
		if (Z_TYPE(proxied_value) == IS_ARRAY) {
			zval *hash_value = zend_symtable_find(Z_ARRVAL(proxied_value),
					member);

			if (hash_value) {
				RETVAL_ZVAL(hash_value, 1, 0);
			}
		}
	} else {
		php_property_proxy_t *proxy;
		php_property_proxy_object_t *proxy_obj;

		if (!Z_ISUNDEF(proxied_value)) {
			convert_to_array(&proxied_value);
			Z_ADDREF(proxied_value);
		} else {
			array_init(&proxied_value);
			set_proxied_value(object, &proxied_value);
		}

		if (!member) {
			member = zend_long_to_str(zend_hash_next_free_element(
					Z_ARRVAL(proxied_value)));
		}

		proxy = php_property_proxy_init(&proxied_value, member);
		zval_ptr_dtor(&proxied_value);

		proxy_obj = php_property_proxy_object_new_ex(NULL, proxy);
		ZVAL_COPY(&proxy_obj->parent, object);
		RETVAL_OBJ(&proxy_obj->zo);
	}

	if (member) {
		zend_string_release(member);
	}

	debug_propro(-1, type == BP_VAR_R ? "dim_read" : "dim_read_ref",
			get_propro(object), offset, return_value);

	return return_value;
}

static int has_dimension(zval *object, zval *offset, int check_empty)
{
	zval proxied_value;
	int exists = 0;

	debug_propro(1, "dim_exists", get_propro(object), offset, NULL);

	ZVAL_UNDEF(&proxied_value);
	get_proxied_value(object, &proxied_value);
	if (Z_ISUNDEF(proxied_value)) {
		exists = 0;
	} else {
		zend_string *zs = zval_get_string(offset);

		if (Z_TYPE(proxied_value) == IS_ARRAY) {
			zval *zentry = zend_symtable_find(Z_ARRVAL(proxied_value), zs);

			if (!zentry) {
				exists = 0;
			} else {
				if (check_empty) {
					exists = !Z_ISNULL_P(zentry);
				} else {
					exists = 1;
				}
			}
		}

		zend_string_release(zs);
	}

	debug_propro(-1, "dim_exists", get_propro(object), offset, NULL);

	return exists;
}

static void write_dimension(zval *object, zval *offset, zval *value)
{
	zval proxied_value;

	debug_propro(1, "dim_write", get_propro(object), offset, value);

	ZVAL_UNDEF(&proxied_value);
	get_proxied_value(object, &proxied_value);

	if (!Z_ISUNDEF(proxied_value)) {
		if (Z_TYPE(proxied_value) == IS_ARRAY) {
			Z_ADDREF(proxied_value);
		} else {
			convert_to_array(&proxied_value);
		}
	} else {
		array_init(&proxied_value);
	}

	SEPARATE_ZVAL(value);
	Z_TRY_ADDREF_P(value);

	if (offset) {
		zend_string *zs = zval_get_string(offset);
		zend_symtable_update(Z_ARRVAL(proxied_value), zs, value);
		zend_string_release(zs);
	} else {
		zend_hash_next_index_insert(Z_ARRVAL(proxied_value), value);
	}

	set_proxied_value(object, &proxied_value);

	debug_propro(-1, "dim_write", get_propro(object), offset, &proxied_value);

	zval_ptr_dtor(&proxied_value);
}

static void unset_dimension(zval *object, zval *offset)
{
	zval proxied_value;

	debug_propro(1, "dim_unset", get_propro(object), offset, NULL);

	ZVAL_UNDEF(&proxied_value);
	get_proxied_value(object, &proxied_value);

	if (Z_TYPE(proxied_value) == IS_ARRAY) {
		zval *o = offset;
		ZEND_RESULT_CODE rv;

		convert_to_string_ex(o);
		rv = zend_symtable_del(Z_ARRVAL(proxied_value), Z_STR_P(o));
		if (SUCCESS == rv) {
			set_proxied_value(object, &proxied_value);
		}

		if (o != offset) {
			zval_ptr_dtor(o);
		}
	}

	debug_propro(-1, "dim_unset", get_propro(object), offset, &proxied_value);
}

ZEND_BEGIN_ARG_INFO_EX(ai_propro_construct, 0, 0, 2)
	ZEND_ARG_INFO(1, object)
	ZEND_ARG_INFO(0, member)
	ZEND_ARG_OBJ_INFO(0, parent, php\\PropertyProxy, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(propro, __construct) {
	zend_error_handling zeh;
	zval *container, *parent = NULL;
	zend_string *member;

	zend_replace_error_handling(EH_THROW, NULL, &zeh);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS(), "zS|O!",
			&container, &member, &parent,
			php_property_proxy_class_entry)) {
		php_property_proxy_object_t *obj;
		zval *ref = get_referenced_zval(container);

		switch (Z_TYPE_P(ref)) {
		case IS_OBJECT:
		case IS_ARRAY:
			break;
		default:
			convert_to_array(ref);
		}
		obj = get_propro(getThis());
		obj->proxy = php_property_proxy_init(container, member);
		if (parent) {
			ZVAL_COPY(&obj->parent, parent);
		}
	}
	zend_restore_error_handling(&zeh);
}

static const zend_function_entry php_property_proxy_method_entry[] = {
	PHP_ME(propro, __construct, ai_propro_construct, ZEND_ACC_PUBLIC)
	{0}
};

static PHP_MINIT_FUNCTION(propro)
{
	zend_class_entry ce = {0};

	INIT_NS_CLASS_ENTRY(ce, "php", "PropertyProxy",
			php_property_proxy_method_entry);
	php_property_proxy_class_entry = zend_register_internal_class(&ce);
	php_property_proxy_class_entry->create_object =	php_property_proxy_object_new;
	php_property_proxy_class_entry->ce_flags |= ZEND_ACC_FINAL;

	memcpy(&php_property_proxy_object_handlers, zend_get_std_object_handlers(),
			sizeof(zend_object_handlers));
	php_property_proxy_object_handlers.offset = XtOffsetOf(php_property_proxy_object_t, zo);
	php_property_proxy_object_handlers.free_obj = destroy_obj;
	php_property_proxy_object_handlers.set = set_proxied_value;
	php_property_proxy_object_handlers.get = get_proxied_value;
	php_property_proxy_object_handlers.cast_object = cast_proxied_value;
	php_property_proxy_object_handlers.read_dimension = read_dimension;
	php_property_proxy_object_handlers.write_dimension = write_dimension;
	php_property_proxy_object_handlers.has_dimension = has_dimension;
	php_property_proxy_object_handlers.unset_dimension = unset_dimension;

	return SUCCESS;
}

PHP_MINFO_FUNCTION(propro)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "Property proxy support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_PROPRO_VERSION);
	php_info_print_table_end();
}

static const zend_function_entry propro_functions[] = {
	{0}
};

zend_module_entry propro_module_entry = {
	STANDARD_MODULE_HEADER,
	"propro",
	propro_functions,
	PHP_MINIT(propro),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(propro),
	PHP_PROPRO_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PROPRO
ZEND_GET_MODULE(propro)
#endif


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
