#include <string.h>
#include <assert.h>
#include "include/private/reflecs.h"

const EcsArrayParams column_arr_params = {
    .element_size = sizeof(EcsSystemColumn)
};

/** Count components in a signature */
static
uint32_t components_count(
    const char *sig)
{
    const char *ptr = sig;
    uint32_t count = 1;

    while ((ptr = strchr(ptr + 1, ','))) {
        count ++;
    }

    return count;
}

/** Parse callback that adds component to the components array for a system */
static
EcsResult add_component(
    EcsWorld *world,
    EcsSystemExprElemKind elem_kind,
    EcsSystemExprOperKind oper_kind,
    const char *component_id,
    void *data)
{
    EcsSystem *system_data = data;
    EcsHandle component = ecs_lookup(world, component_id);
    if (!component) {
        return EcsError;
    }

    if (elem_kind == EcsFromEntity) {
        system_data->from_entity[oper_kind] = ecs_family_add(
            world, system_data->from_entity[oper_kind], component);
    } else {
        system_data->from_component[oper_kind] = ecs_family_add(
            world, system_data->from_component[oper_kind], component);
    }

    EcsSystemColumn *elem;

    if (oper_kind == EcsOperAnd) {
        elem = ecs_array_add(&system_data->columns, &column_arr_params);
        elem->kind = elem_kind;
        elem->oper_kind = EcsOperAnd;
        elem->is.component = component;

    } else if (oper_kind == EcsOperOr) {
        elem = ecs_array_last(system_data->columns, &column_arr_params);
        if (elem->oper_kind == EcsOperAnd) {
            elem->is.family = ecs_family_add(world, 0, component);
        } else {
            if (elem->kind != elem_kind) {
                /* Cannot mix FromEntity and FromComponent in OR */
                goto error;
            }
            elem->is.family = ecs_family_add(world, elem->is.family, component);
        }
        elem->kind = elem_kind;
        elem->oper_kind = EcsOperAnd;

    } else {
        /* Do not add NOT operators to columns */
    }

    return EcsOk;
error:
    return EcsError;
}

static
EcsHandle components_contain(
    EcsWorld *world,
    EcsFamily table_family,
    EcsFamily family,
    EcsHandle *entity_out,
    bool match_all)
{
    EcsArray *components = ecs_map_get(world->family_index, table_family);
    assert(components != NULL);

    uint32_t i, count = ecs_array_count(components);
    for (i = 0; i < count; i ++) {
        EcsHandle h = *(EcsHandle*)ecs_array_get(
            components, &handle_arr_params, i);

        uint64_t row_64 = ecs_map_get64(world->entity_index, h);
        assert(row_64 != 0);

        EcsRow row = ecs_to_row(row_64);
        EcsHandle component = ecs_family_contains(
            world, row.family_id, family, match_all);
        if (component != 0) {
            if (entity_out) *entity_out = h;
            return component;
        }
    }

    return 0;
}

static
bool match_table(
    EcsWorld *world,
    EcsTable *table,
    EcsSystem *system_data)
{
    EcsFamily family, table_family;
    table_family = table->family_id;

    family = system_data->from_entity[EcsOperAnd];
    if (family) {
        if (!ecs_family_contains(world, table_family, family, true)) {
            return false;
        }
    }

    family = system_data->from_entity[EcsOperOr];
    if (family) {
        if (!ecs_family_contains(world, table_family, family, false)) {
            return false;
        }
    }

    family = system_data->from_entity[EcsOperNot];
    if (family) {
        if (ecs_family_contains(world, table_family, family, false)) {
            return false;
        }
    }

    family = system_data->from_component[EcsOperAnd];
    if (family) {
        if (!components_contain(world, table_family, family, NULL, true)) {
            return false;
        }
    }

    family = system_data->from_component[EcsOperOr];
    if (family) {
        if (!components_contain(world, table_family, family, NULL, false)) {
            return false;
        }
    }

    family = system_data->from_component[EcsOperNot];
    if (family) {
        if (components_contain(world, table_family, family, NULL, false)) {
            return false;
        }
    }

    return true;
}

/** Add table to system, compute offsets for system components in table rows */
static
void add_table(
    EcsWorld *world,
    EcsHandle system,
    EcsSystem *system_data,
    EcsTable *table)
{
    int32_t *table_data;
    EcsSystemRef *ref_data = NULL;
    EcsFamily table_family = table->family_id;
    uint32_t count = ecs_array_count(table->rows);
    if (count) {
        table_data = ecs_array_add(
            &system_data->tables, &system_data->table_params);
    } else {
        table_data = ecs_array_add(
            &system_data->inactive_tables, &system_data->table_params);
    }

    /* Table index is at element 0 */
    table_data[0] = ecs_array_get_index(
        world->table_db, &table_arr_params, table);

    /* Index in ref array is at element 1 (0 means no refs) */
    table_data[1] = 0;

    uint32_t i = 2; /* Offsets start after table index and refs index */
    uint32_t ref = 0;
    EcsIter it = ecs_array_iter(system_data->columns, &column_arr_params);
    while (ecs_iter_hasnext(&it)) {
        EcsSystemColumn *column = ecs_iter_next(&it);

        if (column->kind == EcsFromEntity) {
            EcsHandle component = 0;

            if (column->oper_kind == EcsOperAnd) {
                component = column->is.component;
            } else if (column->oper_kind == EcsOperOr) {
                /* Returns first component that matches between families */
                component = ecs_family_contains(
                    world, table_family, column->is.family, false);
            } else {
                assert(0);
            }

            table_data[i] = ecs_table_column_offset(table, component);

        } else if (column->kind == EcsFromComponent) {
            if (!system_data->refs) {
                system_data->refs = ecs_array_new(&system_data->ref_params, 1);
            }

            if (!ref_data) {
                ref_data = ecs_array_add(
                    &system_data->refs, &system_data->ref_params);
                table_data[1] = ecs_array_count(system_data->refs);
            }

            EcsHandle entity = 0;
            EcsHandle component = 0;

            if (column->oper_kind == EcsOperAnd) {
                component = column->is.component;
                EcsFamily family = ecs_family_add(world, 0, component);
                components_contain(world, table_family, family, &entity, true);
            } else if (column->oper_kind == EcsOperOr) {
                component = components_contain(
                    world, table_family, column->is.family, &entity, false);
            } else {
                assert(0);
            }

            ref_data[ref].entity = entity;
            ref_data[ref].component = component;
            ref ++;

            /* Refs are indicated by a negative index */
            table_data[i] = -ref;
        }

        i ++;
    }

    EcsHandle *h = NULL;
    if (system_data->kind == EcsPeriodic || system_data->kind == EcsOnDemand) {
        h = ecs_array_add(&table->periodic_systems, &handle_arr_params);
    } else if (system_data->kind == EcsOnInit) {
        h = ecs_array_add(&table->init_systems, &handle_arr_params);
    } else if (system_data->kind == EcsOnDeinit) {
        h = ecs_array_add(&table->deinit_systems, &handle_arr_params);
    }

    if (h) *h = system;
}

/** Match existing tables against system (table is created before system) */
static
void match_tables(
    EcsWorld *world,
    EcsHandle system,
    EcsSystem *system_data)
{
    EcsIter it = ecs_array_iter(world->table_db, &table_arr_params);
    while (ecs_iter_hasnext(&it)) {
        EcsTable *table = ecs_iter_next(&it);

        if (match_table(world, table, system_data)) {
            add_table(world, system, system_data, table);
        }
    }
}

/** Resolve references */
static
void resolve_refs(
    EcsWorld *world,
    EcsSystem *system_data,
    uint32_t refs_index,
    EcsRows *info)
{
    EcsArray *system_refs = system_data->refs;
    EcsSystemRef *refs = ecs_array_buffer(system_refs);
    uint32_t i, count = ecs_array_count(system_refs);

    for (i = 0; i < count; i ++) {
        EcsSystemRef *ref = &refs[i];
        info->refs[i] = ecs_get(world, ref->entity, ref->component);
    }
}


/* -- Private functions -- */

/** Match new table against system (table is created after system) */
EcsResult ecs_system_notify_create_table(
    EcsWorld *world,
    EcsHandle system,
    EcsTable *table)
{
    EcsSystem *system_data = ecs_get(world, system, EcsSystem_h);
    if (!system_data) {
        return EcsError;
    }

    if (match_table(world, table, system_data)) {
        add_table(world, system, system_data, table);
    }

    return EcsOk;
}

/** Table activation happens when a table was or becomes empty. Deactivated
 * tables are not considered by the system in the main loop. */
void ecs_system_activate_table(
    EcsWorld *world,
    EcsHandle system,
    EcsTable *table,
    bool active)
{
    EcsArray *src_array, *dst_array;
    EcsSystem *system_data = ecs_get(world, system, EcsSystem_h);
    uint32_t table_index = ecs_array_get_index(
        world->table_db, &table_arr_params, table);

    if (active) {
        src_array = system_data->inactive_tables;
        dst_array = system_data->tables;
    } else {
        src_array = system_data->tables;
        dst_array = system_data->inactive_tables;
    }

    uint32_t count = ecs_array_count(src_array);
    int i;
    for (i = 0; i < count; i ++) {
        uint32_t *index = ecs_array_get(
            src_array, &system_data->table_params, i);
        if (*index == table_index) {
            break;
        }
    }

    assert(i != count);

    uint32_t src_count = ecs_array_move_index(
        &dst_array, src_array, &system_data->table_params, i);

    if (active) {
        uint32_t dst_count = ecs_array_count(dst_array);
        if (dst_count == 1 && system_data->enabled) {
            ecs_world_activate_system(world, system, true);
        }
        system_data->tables = dst_array;
    } else {
        if (src_count == 0) {
            ecs_world_activate_system(world, system, false);
        }
        system_data->inactive_tables = dst_array;
    }
}

/** Run subset of the matching entities for a system (used in worker threads) */
void ecs_run_job(
    EcsWorld *world,
    EcsJob *job)
{
    EcsHandle system = job->system;
    EcsSystem *system_data = job->system_data;
    EcsSystemAction action = system_data->action;
    uint32_t table_element_size = system_data->table_params.element_size;
    uint32_t table_index = job->table_index;
    uint32_t start_index = job->start_index;
    uint32_t remaining = job->row_count;
    uint32_t column_count = ecs_array_count(system_data->columns);
    void *refs[column_count];
    char *table_buffer = ecs_array_get(
        system_data->tables, &system_data->table_params, table_index);

    EcsRows info = {
        .world = world,
        .system = system,
        .refs = refs
    };

    do {
        EcsTable *table = ecs_array_get(
            world->table_db, &table_arr_params, *(uint32_t*)table_buffer);
        EcsArray *rows = table->rows;
        void *start = ecs_array_get(rows, &table->row_params, start_index);
        uint32_t count = ecs_array_count(rows);
        uint32_t element_size = table->row_params.element_size;
        uint32_t refs_index = ((uint32_t*)table_buffer)[1];

        info.count = count;
        info.element_size = element_size;
        info.columns = ECS_OFFSET(table_buffer, sizeof(uint32_t) * 2);
        info.first = ECS_OFFSET(start, sizeof(EcsHandle));

        if (refs_index) {
            resolve_refs(world, system_data, refs_index, &info);
        }

        if (remaining >= count) {
            info.last = ECS_OFFSET(info.first, element_size * count);
            table_buffer += table_element_size;
            start_index = 0;
            remaining -= count;
        } else {
            info.last = ECS_OFFSET(info.first, element_size * remaining);
            remaining = 0;
        }

        action(&info);
    } while (remaining);
}


/** Run system on a single row */
void ecs_system_notify(
    EcsWorld *world,
    EcsHandle system,
    EcsSystem *system_data,
    EcsTable *table,
    uint32_t table_index,
    uint32_t row_index)
{
    EcsSystemAction action = system_data->action;
    uint32_t t, table_count = ecs_array_count(system_data->tables);
    uint32_t column_count = ecs_array_count(system_data->columns);
    void *refs[column_count];

    EcsRows info = {
        .world = world,
        .system = system,
        .param = NULL,
        .refs = refs
    };

    for (t = 0; t < table_count; t++) {
        int32_t *table_data = ecs_array_get(
            system_data->tables, &system_data->table_params, t);
        int32_t sys_table_index = table_data[0];

        if (sys_table_index == table_index) {
            EcsArray *rows = table->rows;
            void *row = ecs_array_get(rows, &table->row_params, row_index);
            int32_t refs_index = table_data[1];
            info.element_size = table->row_params.element_size;
            info.columns = ECS_OFFSET(table_data, sizeof(int32_t) * 2);
            info.first = ECS_OFFSET(row, sizeof(EcsHandle));
            info.last = ECS_OFFSET(info.first, info.element_size);
            if (refs_index) {
                resolve_refs(world, system_data, refs_index, &info);
            }
            action(&info);
            break;
        }
    }
}


/* -- Public API -- */

void ecs_run_system(
    EcsWorld *world,
    EcsHandle system,
    void *param)
{
    EcsSystem *system_data = ecs_get(world, system, EcsSystem_h);
    if (!system_data->enabled) {
        return;
    }

    EcsSystemAction action = system_data->action;
    EcsArray *tables = system_data->tables;
    EcsArray *table_db = world->table_db;
    uint32_t table_count = ecs_array_count(tables);
    uint32_t column_count = ecs_array_count(system_data->columns);
    uint32_t element_size = system_data->table_params.element_size;
    char *table_buffer = ecs_array_buffer(tables);
    char *last = ECS_OFFSET(table_buffer, element_size * table_count);
    void *refs[column_count];

    EcsRows info = {
        .world = world,
        .system = system,
        .param = param,
        .refs = refs
    };

    for (; table_buffer < last; table_buffer += element_size) {
        int32_t table_index = ((int32_t*)table_buffer)[0];
        int32_t refs_index = ((int32_t*)table_buffer)[1];
        EcsTable *table = ecs_array_get(
            table_db, &table_arr_params, table_index);
        EcsArray *rows = table->rows;
        void *buffer = ecs_array_buffer(rows);
        uint32_t count = ecs_array_count(rows);

        if (refs_index) {
            resolve_refs(world, system_data, refs_index, &info);
        }

        info.count = count;
        info.element_size = table->row_params.element_size;
        info.first = ECS_OFFSET(buffer, sizeof(EcsHandle));
        info.last = ECS_OFFSET(info.first, info.element_size * count);
        info.columns = ECS_OFFSET(table_buffer, sizeof(uint32_t) * 2);
        action(&info);
    }
}

EcsHandle ecs_new_system(
    EcsWorld *world,
    const char *id,
    EcsSystemKind kind,
    const char *sig,
    EcsSystemAction action)
{
    uint32_t count = components_count(sig);
    if (!count) {
        return 0;
    }

    EcsHandle result = ecs_new_w_family(world, world->system_family);

    EcsSystem *system_data = ecs_get(world, result, EcsSystem_h);
    system_data->action = action;
    system_data->enabled = true;
    memset(system_data->from_entity, 0, sizeof(system_data->from_entity));
    memset(system_data->from_component, 0, sizeof(system_data->from_component));
    system_data->table_params.element_size = sizeof(int32_t) * (count + 2);
    system_data->table_params.move_action = NULL;
    system_data->ref_params.element_size = sizeof(EcsSystemRef) * count;
    system_data->ref_params.move_action = NULL;
    system_data->refs = NULL;
    system_data->tables = ecs_array_new(
        &system_data->table_params, ECS_SYSTEM_INITIAL_TABLE_COUNT);
    system_data->inactive_tables = ecs_array_new(
        &system_data->table_params, ECS_SYSTEM_INITIAL_TABLE_COUNT);
    system_data->columns = ecs_array_new(&column_arr_params, count);
    system_data->kind = kind;
    system_data->jobs = NULL;

    EcsId *id_data = ecs_get(world, result, EcsId_h);
    id_data->id = id;

    if (ecs_parse_component_expr(
        world, sig, add_component, system_data) != EcsOk)
    {
        ecs_delete(world, result);
        return 0;
    }

    match_tables(world, result, system_data);

    if (kind == EcsPeriodic) {
        EcsHandle *elem;
        if (ecs_array_count(system_data->tables)) {
            elem = ecs_array_add(&world->periodic_systems, &handle_arr_params);
        } else {
            elem = ecs_array_add(&world->inactive_systems, &handle_arr_params);
        }
        *elem = result;
    } else {
        EcsHandle *elem = ecs_array_add(
            &world->other_systems, &handle_arr_params);
        *elem = result;
    }

    return result;
}

EcsResult ecs_enable(
    EcsWorld *world,
    EcsHandle system,
    bool enabled)
{
    EcsSystem *system_data = ecs_get(world, system, EcsSystem_h);
    if (!system_data) {
        return EcsError;
    }

    if (enabled) {
        if (!system_data->enabled) {
            if (ecs_array_count(system_data->tables)) {
                ecs_world_activate_system(world, system, true);
            }
        }
    } else {
        if (system_data->enabled) {
            if (ecs_array_count(system_data->tables)) {
                ecs_world_activate_system(world, system, false);
            }
        }
    }

    system_data->enabled = enabled;

    return EcsOk;
}

bool ecs_is_enabled(
    EcsWorld *world,
    EcsHandle system)
{
    EcsSystem *system_data = ecs_get(world, system, EcsSystem_h);
    if (system_data) {
        return system_data->enabled;
    } else {
        return true;
    }
}