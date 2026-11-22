#pragma once

#include "extension/xmq_extension.h"

#define XMQ_DB_SINK_SETTINGS \
    {.name = "database_uri", .label = "Database URI", .description = "Connection URI used when Database type is set to Use saved URI. May contain credentials.", .type = XMQ_SETTING_SECRET}, \
    {.name = "db_type", .label = "Database type", .description = "Choose a driver installed on the broker", .type = XMQ_SETTING_CHOICE, .default_value = "Use saved URI", .choices = "Use saved URI,postgresql,mysql,oracle,sqlite3,mssql"}, \
    {.name = "db_host", .label = "Database host", .description = "Hostname or address reachable from the broker", .type = XMQ_SETTING_STRING}, \
    {.name = "db_port", .label = "Database port", .description = "Leave blank to use the driver default", .type = XMQ_SETTING_INTEGER, .pattern = "[0-9]{1,5}"}, \
    {.name = "db_name", .label = "Database name or service", .description = "Name of the target database or the service name used by the driver", .type = XMQ_SETTING_STRING}, \
    {.name = "db_user", .label = "Database user", .description = "Account used to connect and execute the INSERT", .type = XMQ_SETTING_STRING}, \
    {.name = "db_password", .label = "Database password", .description = "Password for the database account, if required by the database", .type = XMQ_SETTING_SECRET}, \
    {.name = "db_path", .label = "SQLite file", .description = "Absolute path on the broker", .type = XMQ_SETTING_STRING}, \
    {.name = "insert_sql", .label = "INSERT statement", .description = "Parameterized INSERT; bind the exported JSON as :fields", .type = XMQ_SETTING_STRING}
