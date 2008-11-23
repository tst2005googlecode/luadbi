#include "dbd_mysql.h"

static lua_push_type_t mysql_to_lua_push(unsigned int mysql_type) {
    lua_push_type_t lua_type;

    switch(mysql_type) {
    case MYSQL_TYPE_NULL:
	lua_type = LUA_PUSH_NIL;
	break;

    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:	
	lua_type =  LUA_PUSH_INTEGER;
	break;

    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_LONGLONG:
	lua_type = LUA_PUSH_NUMBER;
	break;

    default:
	lua_type = LUA_PUSH_STRING;
    }

    return lua_type;
} 

static int statement_close(lua_State *L) {
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_MYSQL_STATEMENT);

    if (statement->metadata) {
	mysql_free_result(statement->metadata);
    }

    if (statement->stmt) {
	mysql_stmt_close(statement->stmt);	
    }

    return 1;    
}

static int statement_execute(lua_State *L) {
    int n = lua_gettop(L);
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_MYSQL_STATEMENT); 
    int num_bind_params = n - 1;
    
    MYSQL_BIND *bind = NULL;
    MYSQL_RES *metadata = NULL;

    char *error_message = NULL;

    int p;

    bind = malloc(sizeof(MYSQL_BIND) * num_bind_params);
    memset(bind, 0, sizeof(MYSQL_BIND) * num_bind_params);

    for (p = 2; p <= n; p++) {
	int type = lua_type(L, p);
	int i = p - 2;

	const char *str = NULL;
	size_t str_len;

	double num;

	switch(type) {
	    case LUA_TNIL:
		bind[i].buffer_type = MYSQL_TYPE_NULL;
		bind[i].is_null = (my_bool*)1;
		break;

	    case LUA_TNUMBER:
		num = luaL_checknumber(L, p);

		bind[i].buffer_type = MYSQL_TYPE_DOUBLE;
		bind[i].is_null = (my_bool*)0;
		bind[i].buffer = (char *)&num;
		bind[i].length = 0;
		break;

	    case LUA_TSTRING:
		str = luaL_checklstring(L, p, &str_len);

		bind[i].buffer_type = MYSQL_TYPE_STRING;
		bind[i].is_null = (my_bool*)0;
		bind[i].buffer = (char *)str;
		bind[i].length = &str_len;
		break;

	    default:
		error_message = "Binding unknown or unsupported type"; 
		goto cleanup;
	}
    }

    if (mysql_stmt_bind_param(statement->stmt, bind)) {
	error_message = "Error binding statement parameters: %s";
	goto cleanup;
    }

    if (mysql_stmt_execute(statement->stmt)) {
	error_message = "Error executing statement: %s";
	goto cleanup;
    }

    metadata = mysql_stmt_result_metadata(statement->stmt);

cleanup:
    if (bind)
	free(bind);

    if (error_message) {
	luaL_error(L, error_message, mysql_stmt_error(statement->stmt));
	return 0;
    }

    statement->metadata = metadata;

    return 1;
}

static int statement_fetch_impl(lua_State *L, int named_columns) {
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_MYSQL_STATEMENT);
    int column_count;
    MYSQL_BIND *bind = NULL;
    const char *error_message = NULL;

    if (!statement->stmt) {
	luaL_error(L, "fetch called before execute");
	lua_pushnil(L);
	return 1;
    }

    if (!statement->metadata) {
	lua_pushnil(L);
	return 1;
    }

    column_count = mysql_num_fields(statement->metadata);

    if (column_count > 0) {
	int i;
	MYSQL_FIELD *fields;

        bind = malloc(sizeof(MYSQL_BIND) * column_count);
        memset(bind, 0, sizeof(MYSQL_BIND) * column_count);

	fields = mysql_fetch_fields(statement->metadata);

	for (i = 0; i < column_count; i++) {
	    unsigned int length = fields[i].length;

	    char *buffer = (char *)malloc(length);
	    memset(buffer, 0, length);

	    bind[i].buffer_type = fields[i].type; 
	    bind[i].buffer = buffer;
	    bind[i].buffer_length = length;
	}

	if (mysql_stmt_bind_result(statement->stmt, bind)) {
	    error_message = "Error binding results: %s";
	    goto cleanup;
	}

	if (!mysql_stmt_fetch(statement->stmt)) {
	    int d = 1;

	    lua_newtable(L);
	    for (i = 0; i < column_count; i++) {
		lua_push_type_t lua_push = mysql_to_lua_push(fields[i].type);
		const char *name = fields[i].name;

		if (lua_push == LUA_PUSH_NIL) {
		    if (named_columns) {
			LUA_PUSH_ATTRIB_NIL(name);
		    } else {
			LUA_PUSH_ARRAY_NIL(d);
		    }
		} else if (lua_push == LUA_PUSH_INTEGER) {
		    if (named_columns) {
			LUA_PUSH_ATTRIB_INT(name, *(int *)(bind[i].buffer)); 
		    } else {
			LUA_PUSH_ARRAY_INT(d, *(int *)(bind[i].buffer)); 
		    }
		} else if (lua_push == LUA_PUSH_NUMBER) {
		    if (named_columns) {
			LUA_PUSH_ATTRIB_FLOAT(name, *(double *)(bind[i].buffer));
		    } else {
			LUA_PUSH_ARRAY_FLOAT(d, *(double *)(bind[i].buffer));
		    }
		} else if (lua_push == LUA_PUSH_STRING) {
		    if (named_columns) {
			LUA_PUSH_ATTRIB_STRING(name, bind[i].buffer);
		    } else {
			LUA_PUSH_ARRAY_STRING(d, bind[i].buffer);
		    }
		} else if (lua_push == LUA_PUSH_BOOLEAN) {
		    if (named_columns) {
			LUA_PUSH_ATTRIB_BOOL(name, *(int *)(bind[i].buffer));
		    } else {
			LUA_PUSH_ARRAY_BOOL(d, *(int *)(bind[i].buffer));
		    }
		} else {
		    luaL_error(L, "Unknown push type in result set");
		}
	    }
	} else {
	    lua_pushnil(L);	    
	}
    }

cleanup:
    if (bind) {
	int i;

	for (i = 0; i < column_count; i++) {
	    free(bind[i].buffer);
	}

	free(bind);
    }

    if (error_message) {
        luaL_error(L, error_message, mysql_stmt_error(statement->stmt));
        return 0;
    }

    return 1;    
}


static int statement_fetch(lua_State *L) {
    return statement_fetch_impl(L, 0);
}

static int statement_fetchtable(lua_State *L) {
    return statement_fetch_impl(L, 1);
}

static int statement_gc(lua_State *L) {
    /* always free the handle */
    statement_close(L);

    return 0;
}


static const luaL_Reg statement_methods[] = {
    {"close", statement_close},
    {"execute", statement_execute},
    {"fetch", statement_fetch},
    {"fetchtable", statement_fetchtable},
    {NULL, NULL}
};

static const luaL_Reg statement_class_methods[] = {
    {NULL, NULL}
};

int dbd_mysql_statement_create(lua_State *L, connection_t *conn, const char *sql_query) { 
    unsigned long sql_len = strlen(sql_query);

    statement_t *statement = NULL;

    MYSQL_STMT *stmt = mysql_stmt_init(conn->mysql);

    if (!stmt) {
	luaL_error(L, "Error allocating statement handle: %s", mysql_error(conn->mysql));
	return 0;
    }

    if (mysql_stmt_prepare(stmt, sql_query, sql_len)) {
	luaL_error(L, "Error preparing statement handle: %s", mysql_stmt_error(stmt));
	return 0;
    }

    statement = (statement_t *)lua_newuserdata(L, sizeof(statement_t));
    statement->mysql = conn->mysql;
    statement->stmt = stmt;
    statement->metadata = NULL;

    luaL_getmetatable(L, DBD_MYSQL_STATEMENT);
    lua_setmetatable(L, -2);

    return 1;
} 

int dbd_mysql_statement(lua_State *L) {
    luaL_newmetatable(L, DBD_MYSQL_STATEMENT);
    luaL_register(L, 0, statement_methods);
    lua_pushvalue(L,-1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, statement_gc);
    lua_setfield(L, -2, "__gc");

    luaL_register(L, DBD_MYSQL_STATEMENT, statement_class_methods);

    return 1;    
}