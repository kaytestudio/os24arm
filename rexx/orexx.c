/* ============================================================================
 * rexx/orexx.c  —  Object REXX Core Implementation
 *
 * Implementation of Object REXX object model, classes, and message passing.
 * ========================================================================== */

#include "orexx.h"
#include "rexx.h"
#include <stddef.h>

/* ── Built-in Classes (initialized in orexx_init) ───────────────────────── */

ORexxClass *orexx_class_Object    = NULL;
ORexxClass *orexx_class_Class     = NULL;
ORexxClass *orexx_class_String    = NULL;
ORexxClass *orexx_class_Array     = NULL;
ORexxClass *orexx_class_Directory = NULL;
ORexxClass *orexx_class_Method    = NULL;

/* ── Runtime allocation ─────────────────────────────────────────────────── */

static void* orexx_malloc(size_t size)
{
    /* Keep allocations local until this runtime is wired into the kernel. */
    static char heap[1024 * 1024];  /* 1MB static heap */
    static size_t offset = 0;

    if (offset + size > sizeof(heap))
        return NULL;

    void *ptr = &heap[offset];
    offset += size;
    return ptr;
}

static void orexx_free(void *ptr)
{
    /* The arena allocator cannot reclaim individual objects. */
    (void)ptr;
}

/* ── String Utilities ────────────────────────────────────────────────────── */

static char* orexx_strdup(const char *str)
{
    if (!str) return NULL;

    size_t len = 0;
    while (str[len]) len++;

    char *copy = orexx_malloc(len + 1);
    if (!copy) return NULL;

    for (size_t i = 0; i <= len; i++)
        copy[i] = str[i];

    return copy;
}

static int orexx_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Object lifecycle */

ORexxObject* orexx_object_new(ORexxClass *class)
{
    if (!class) return NULL;

    ORexxObject *obj = orexx_malloc(sizeof(ORexxObject));
    if (!obj) return NULL;

    orexx_object_init(obj, class);
    return obj;
}

void orexx_object_init(ORexxObject *obj, ORexxClass *class)
{
    if (!obj) return;

    obj->class = class;
    obj->instance_vars = NULL;
    obj->ref_count = 1;
    obj->flags = 0;
}

void orexx_object_free(ORexxObject *obj)
{
    if (!obj) return;

    /* Free instance variables */
    if (obj->instance_vars) {
        orexx_free(obj->instance_vars);
        obj->instance_vars = NULL;
    }

    orexx_free(obj);
}

void orexx_object_retain(ORexxObject *obj)
{
    if (obj) obj->ref_count++;
}

void orexx_object_release(ORexxObject *obj)
{
    if (!obj) return;

    if (--obj->ref_count == 0) {
        orexx_object_free(obj);
    }
}

/* Class management */

ORexxClass* orexx_class_new(const char *name, ORexxClass *superclass)
{
    ORexxClass *class = orexx_malloc(sizeof(ORexxClass));
    if (!class) return NULL;

    orexx_object_init(&class->base, orexx_class_Class);

    class->name = orexx_strdup(name);
    class->superclass = superclass;
    class->methods = NULL;
    class->method_count = 0;
    class->instance_size = sizeof(ORexxObject);

    return class;
}

void orexx_class_add_method(ORexxClass *class, const char *name,
                            void *code, uint32_t flags)
{
    if (!class || !name || !code) return;

    /* Allocate new method */
    ORexxMethod *method = orexx_malloc(sizeof(ORexxMethod));
    if (!method) return;

    method->name = orexx_strdup(name);
    method->code = code;
    method->flags = flags;
    method->owner = class;

    /* Methods are stored in a linear array and searched by name. */
    ORexxMethod *new_methods = orexx_malloc(
        sizeof(ORexxMethod) * (class->method_count + 1));

    if (new_methods) {
        for (uint32_t i = 0; i < class->method_count; i++) {
            new_methods[i] = class->methods[i];
        }
        new_methods[class->method_count] = *method;

        if (class->methods) orexx_free(class->methods);
        class->methods = new_methods;
        class->method_count++;
    }
}

ORexxMethod* orexx_class_lookup_method(ORexxClass *class, const char *name)
{
    if (!class || !name) return NULL;

    /* Search this class */
    for (uint32_t i = 0; i < class->method_count; i++) {
        if (orexx_strcmp(class->methods[i].name, name) == 0) {
            return &class->methods[i];
        }
    }

    /* Search superclass chain */
    if (class->superclass) {
        return orexx_class_lookup_method(class->superclass, name);
    }

    return NULL;  /* Method not found */
}

/* Message sending */

ORexxObject* orexx_send_message(ORexxObject *receiver, const char *selector,
                                ORexxObject **args, uint32_t arg_count)
{
    if (!receiver || !selector) return NULL;

    /* Look up method in receiver's class */
    ORexxMethod *method = orexx_class_lookup_method(receiver->class, selector);

    if (!method) {
        /* doesNotUnderstand: dispatch is not implemented yet. */
        return NULL;
    }

    /* Method bodies are not executable yet; method->code is not dispatched. */
    (void)args;
    (void)arg_count;

    return NULL;  /* Method bodies are not executed yet. */
}

/* Instance variables */

void orexx_set_instance_var(ORexxObject *obj, const char *name,
                            ORexxObject *value)
{
    /* Instances do not have variable storage yet. */
    (void)obj;
    (void)name;
    (void)value;
}

ORexxObject* orexx_get_instance_var(ORexxObject *obj, const char *name)
{
    /* Instances do not have variable storage yet. */
    (void)obj;
    (void)name;
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
   TYPE CHECKING
   ══════════════════════════════════════════════════════════════════════════ */

int orexx_is_instance_of(ORexxObject *obj, ORexxClass *class)
{
    if (!obj || !class) return 0;
    return obj->class == class;
}

int orexx_is_kind_of(ORexxObject *obj, ORexxClass *class)
{
    if (!obj || !class) return 0;

    ORexxClass *current = obj->class;
    while (current) {
        if (current == class) return 1;
        current = current->superclass;
    }

    return 0;
}

/* Initialization */

void orexx_init(void)
{
    /* Object is the root of all classes */
    orexx_class_Object = orexx_malloc(sizeof(ORexxClass));
    if (orexx_class_Object) {
        orexx_class_Object->name = orexx_strdup("Object");
        orexx_class_Object->superclass = NULL;
        orexx_class_Object->methods = NULL;
        orexx_class_Object->method_count = 0;
        orexx_class_Object->instance_size = sizeof(ORexxObject);
    }

    /* Class is the metaclass */
    orexx_class_Class = orexx_class_new("Class", orexx_class_Object);

    /* Built-in types */
    orexx_class_String    = orexx_class_new("String", orexx_class_Object);
    orexx_class_Array     = orexx_class_new("Array", orexx_class_Object);
    orexx_class_Directory = orexx_class_new("Directory", orexx_class_Object);
    orexx_class_Method    = orexx_class_new("Method", orexx_class_Object);

    /* Built-in classes currently have no methods. */
}

void orexx_shutdown(void)
{
    /* The process-wide arena is reclaimed when the process exits. */
}
