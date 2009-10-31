//////////////////////////////////////////////////////////////////////
// Implement the archive{write} object.
//////////////////////////////////////////////////////////////////////

#include <archive.h>
#include <ctype.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>

#include "private_archive_write.h"
#include "private_archive_entry.h"
#include "private_archive_registry.h"

#define err(...) (luaL_error(L, __VA_ARGS__))
#define rel_idx(relative, idx) ((idx) < 0 ? (idx) + (relative) : (idx))

static __LA_SSIZE_T ar_write_cb(struct archive * ar,
                                void *opaque,
                                const void *buff, size_t len);

static int __ref_count = 0;

//////////////////////////////////////////////////////////////////////
// For debugging GC issues.
static int ar_ref_count(lua_State *L) {
    lua_pushnumber(L, __ref_count);
    lua_settop(L, -1);
    return 1;
}

//////////////////////////////////////////////////////////////////////
// Constructor:
static int ar_write(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    struct archive** self_ref = (struct archive**)
        lua_newuserdata(L, sizeof(struct archive*)); // {ud}
    *self_ref = NULL;
    luaL_getmetatable(L, AR_WRITE); // {ud}, [write]
    lua_setmetatable(L, -2); // {ud}
    __ref_count++;
    *self_ref = archive_write_new();

    // Register it in the weak metatable:
    lua_archive_register(L, *self_ref);

    // Create an environment to store a reference to the printer:
    lua_createtable(L, 1, 0); // {ud}, {}
    lua_getfield(L, 1, "printer"); // {ud}, {}, fn
    if ( ! lua_isfunction(L, -1) ) err("required parameter 'printer' must be a function");
    lua_setfield(L, -2, "printer");
    lua_setfenv(L, -2); // {ud}

    // Extract various fields and prepare the archive:
    lua_getfield(L, 1, "bytes_per_block");
    if ( ! lua_isnil(L, -1) &&
         ARCHIVE_OK != archive_write_set_bytes_per_block(*self_ref, lua_tointeger(L, -1)) )
    {
        err("archive_write_set_bytes_per_block: %s", archive_error_string(*self_ref));
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "bytes_in_last_block");
    if ( ! lua_isnil(L, -1) &&
         ARCHIVE_OK != archive_write_set_bytes_in_last_block(*self_ref, lua_tointeger(L, -1)) )
    {
        err("archive_write_set_bytes_in_last_block: %s", archive_error_string(*self_ref));
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "skip_file");
    if ( ! lua_isnil(L, -1) ) {
        dev_t dev;
        ino_t ino;

        if ( LUA_TTABLE != lua_type(L, -1) ) {
            err("skip_file member must be a table object");
        }

        lua_getfield(L, -1, "dev");
        if ( ! lua_isnumber(L, -1) ) {
            err("skip_file.dev member must be a number");
        }
        dev = (dev_t)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "ino");
        if ( ! lua_isnumber(L, -1) ) {
            err("skip_file.ino member must be a number");
        }
        ino = (ino_t)lua_tonumber(L, -1);
        lua_pop(L, 1);

        if ( ARCHIVE_OK != archive_write_set_skip_file(*self_ref, dev, ino) ) {
            err("archive_write_set_skip_file: %s", archive_error_string(*self_ref));
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "format");
    if ( ! lua_isnil(L, -1) ) {
        static struct {
            const char *name;
            int (*setter)(struct archive *);
        } names[] = {
            /* Copied from archive_write_set_format_by_name.c */
            { "ar",         archive_write_set_format_ar_bsd },
            { "arbsd",      archive_write_set_format_ar_bsd },
            { "argnu",      archive_write_set_format_ar_svr4 },
            { "arsvr4",     archive_write_set_format_ar_svr4 },
            { "cpio",       archive_write_set_format_cpio },
            { "mtree",      archive_write_set_format_mtree },
            { "newc",       archive_write_set_format_cpio_newc },
            { "odc",        archive_write_set_format_cpio },
            { "pax",        archive_write_set_format_pax },
            { "posix",      archive_write_set_format_pax },
            { "shar",       archive_write_set_format_shar },
            { "shardump",   archive_write_set_format_shar_dump },
            { "ustar",      archive_write_set_format_ustar },
            /* New ones to more closely match the C API */
            { "ar_bsd",     archive_write_set_format_ar_bsd },
            { "ar_svr4",    archive_write_set_format_ar_svr4 },
            { "cpio_newc",  archive_write_set_format_cpio_newc },
            { "pax_restricted", archive_write_set_format_pax_restricted },
            { "shar_dump",  archive_write_set_format_shar_dump },
            { NULL,         NULL }
        };
        int idx = 0;
        const char* name = lua_tostring(L, -1);
        for ( ;; idx++ ) {
            if ( names[idx].name == NULL ) {
                err("archive_write_set_format_*: No such format '%s'", name);
            }
            if ( strcmp(name, names[idx].name) == 0 ) break;
        }
        if ( ARCHIVE_OK != (names[idx].setter)(*self_ref) ) {
            err("archive_write_set_format_*: %s", archive_error_string(*self_ref));
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "compression");
    if ( ! lua_isnil(L, -1) ) {
        static struct {
            const char *name;
            int (*setter)(struct archive *);
        } names[] = {
            { "bzip2",    archive_write_set_compression_bzip2 },
            { "compress", archive_write_set_compression_compress },
            { "gzip",     archive_write_set_compression_gzip },
            { "lzma",     archive_write_set_compression_lzma },
            { "xz",       archive_write_set_compression_xz },
            { NULL,       NULL }
        };
        int idx = 0;
        const char* name = lua_tostring(L, -1);
        for ( ;; idx++ ) {
            if ( names[idx].name == NULL ) {
                err("archive_write_set_compression_*: No such compression '%s'", name);
            }
            if ( strcmp(name, names[idx].name) == 0 ) break;
        }
        if ( ARCHIVE_OK != (names[idx].setter)(*self_ref) ) {
            err("archive_write_set_compression_*: %s", archive_error_string(*self_ref));
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "options");
    if ( ! lua_isnil(L, -1) &&
         ARCHIVE_OK != archive_write_set_options(*self_ref, lua_tostring(L, -1)) )
    {
        err("archive_write_set_options: %s",  archive_error_string(*self_ref));
    }
    lua_pop(L, 1);


    if ( ARCHIVE_OK != archive_write_open(*self_ref, L, NULL, &ar_write_cb, NULL) ) {
        err("archive_write_open: %s", archive_error_string(*self_ref));
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////
// Precondition: archive{write} is at the top of the stack, and idx is
// the index to the argument for which to pass to printer exists.  If
// idx is zero, nil is passed into printer.
static void ar_write_get_printer(lua_State *L, int self_idx) {
    lua_getfenv(L, self_idx);        // {env}
    lua_getfield(L, -1, "printer"); // {env}, printer
    lua_insert(L, -2);              // printer, {env}
    lua_pop(L, 1);                  // printer
}

//////////////////////////////////////////////////////////////////////
static int ar_write_destroy(lua_State *L) {
    fprintf(stderr, "ar_write_destroy\n");
    struct archive** self_ref = ar_write_check(L, 1);
    if ( NULL == *self_ref ) return 0;

    if ( ARCHIVE_OK != archive_write_close(*self_ref) ) {
        lua_pushfstring(L, "archive_write_close: %s", archive_error_string(*self_ref));
        archive_write_finish(*self_ref);
        __ref_count--;
        *self_ref = NULL;
        lua_error(L);
    }

    ar_write_get_printer(L, 1); // {self}, printer
    lua_pushvalue(L, 1); // {self}, printer, {self}
    lua_pushnil(L); // {self}, printer, {self}, nil
    lua_call(L, 2, 1); // {self}, result

    if ( ARCHIVE_OK != archive_write_finish(*self_ref) ) {
        luaL_error(L, "archive_write_finish: %s", archive_error_string(*self_ref));
    }
    __ref_count--;
    *self_ref = NULL;

    return 0;
}

//////////////////////////////////////////////////////////////////////
static __LA_SSIZE_T ar_write_cb(struct archive * self,
                                void *opaque,
                                const void *buff, size_t len)
{
    lua_State* L = (lua_State*)opaque;

    // We are missing!?
    if ( ! lua_archive_get(L, self) ) {
        archive_set_error(self, 0,
                          "InternalError: write callback called on archive that should already have been garbage collected!");
        return -1;
    }

    ar_write_get_printer(L, -1); // {ud}, printer
    lua_pushvalue(L, -2); // {ud}, printer, {ud}
    lua_pushlstring(L, (const char *)buff, len); // {ud}, printer, {ud}, str

    if ( 0 != lua_pcall(L, 2, 1, 0) ) { // {ud}, "err"
        archive_set_error(self, 0, "%s", lua_tostring(L, -1));
        lua_pop(L, 2); // <nothing>
        return -1;
    }
    size_t result = lua_tointeger(L, -1); // {ud}, result
    lua_pop(L, 2); // <nothing>

    return result;
}

//////////////////////////////////////////////////////////////////////
static int ar_write_header(lua_State *L) {
    struct archive* self = *ar_write_check(L, 1);
    if ( NULL == self ) err("NULL archive{write}!");

    struct archive_entry* entry = *ar_entry_check(L, 2);
    if ( NULL == entry ) err("NULL archive{entry}!");

    if ( ARCHIVE_OK != archive_write_header(self, entry) ) {
        err("archive_write_header: %s", archive_error_string(self));
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////
static int ar_write_data(lua_State *L) {
    struct archive* self = *ar_write_check(L, 1);
    if ( NULL == self ) err("NULL archive{write}!");

    size_t len;
    const char* data = lua_tolstring(L, 2, &len);

    size_t wrote = archive_write_data(self, data, len);
    if ( -1 == wrote ) {
        err("archive_write_data: %s", archive_error_string(self));
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////
// Precondition: top of the stack contains a table for which we will
// append our "static" methods.
//
// Postcondition: 'write' method is registered in the table at the top
// of the stack, and the archive{write} metatable is registered.
//////////////////////////////////////////////////////////////////////
int lua_archive_write(lua_State *L) {
    luaL_checktype(L, LUA_TTABLE, -1); // {class}

    static luaL_reg fns[] = {
        { "write",  ar_write },
        { "_write_ref_count", ar_ref_count },
        { NULL, NULL }
    };
    luaL_register(L, NULL, fns); // {class}

    luaL_newmetatable(L, AR_WRITE); // {class}, {meta}

    lua_pushvalue(L, -1); // {class}, {meta}, {meta}
    lua_setfield(L, -2, "__index"); // {class}, {meta}

    static luaL_reg m_fns[] = {
        { "header",  ar_write_header },
        { "data",    ar_write_data },
        { "close",   ar_write_destroy },
        { "__gc",    ar_write_destroy },
        { NULL, NULL }
    };
    luaL_register(L, NULL, m_fns); // {class}, {meta}

    lua_pop(L, 1); // {class}

    return 0;
}
