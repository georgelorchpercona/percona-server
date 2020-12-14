/* Copyright (c) 2016, 2020, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <algorithm>  // std::min

#include <stdio.h>
#include <sys/types.h>
#include <boost/optional/optional.hpp>
#include <new>

#include <mysql/components/my_service.h>
#include <mysql/components/services/udf_metadata.h>
#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysql/plugin.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"  // THD

#define MAX_KEYRING_UDF_KEY_LENGTH 16384
#define MAX_KEYRING_UDF_KEY_TEXT_LENGTH MAX_KEYRING_UDF_KEY_LENGTH
size_t KEYRING_UDF_KEY_TYPE_LENGTH = 128;
namespace {
SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(mysql_udf_metadata) *udf_metadata_service = nullptr;
const char *utf8mb4 = "utf8mb4";
char *charset = const_cast<char *>(utf8mb4);
const char *type = "charset";
}  // namespace
#ifdef WIN32
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PLUGIN_EXPORT extern "C"
#endif

static bool is_keyring_udf_initialized = false;

static int keyring_udf_init(void *) {
  DBUG_TRACE;
  is_keyring_udf_initialized = true;
  reg_srv = mysql_plugin_registry_acquire();
  my_h_service h_udf_metadata_service;
  if (reg_srv->acquire("mysql_udf_metadata", &h_udf_metadata_service)) return 1;
  udf_metadata_service = reinterpret_cast<SERVICE_TYPE(mysql_udf_metadata) *>(
      h_udf_metadata_service);

  return 0;
}

static int keyring_udf_deinit(void *) {
  DBUG_TRACE;
  is_keyring_udf_initialized = false;
  using udf_metadata_t = SERVICE_TYPE_NO_CONST(mysql_udf_metadata);
  if (udf_metadata_service)
    reg_srv->release(reinterpret_cast<my_h_service>(
        const_cast<udf_metadata_t *>(udf_metadata_service)));
  mysql_plugin_registry_release(reg_srv);
  return 0;
}

struct st_mysql_daemon keyring_udf_decriptor = {MYSQL_DAEMON_INTERFACE_VERSION};

/*
  Plugin library descriptor
*/

mysql_declare_plugin(keyring_udf){
    MYSQL_DAEMON_PLUGIN,
    &keyring_udf_decriptor,
    "keyring_udf",
    PLUGIN_AUTHOR_ORACLE,
    "Keyring UDF plugin",
    PLUGIN_LICENSE_GPL,
    keyring_udf_init,   /* Plugin Init */
    nullptr,            /* Plugin check uninstall */
    keyring_udf_deinit, /* Plugin Deinit */
    0x0100 /* 1.0 */,
    nullptr, /* status variables                */
    nullptr, /* system variables                */
    nullptr, /* config options                  */
    0,       /* flags                           */
} mysql_declare_plugin_end;

static bool get_current_user(std::string *current_user) {
  THD *thd = current_thd;
  MYSQL_SECURITY_CONTEXT sec_ctx;
  LEX_CSTRING user, host;

  if (thd_get_security_context(thd, &sec_ctx) ||
      security_context_get_option(sec_ctx, "priv_user", &user) ||
      security_context_get_option(sec_ctx, "priv_host", &host))
    return true;

  if (user.length) current_user->append(user.str, user.length);
  assert(host.length);
  current_user->append("@").append(host.str, host.length);

  return false;
}

enum what_to_validate {
  VALIDATE_KEY = 1,
  VALIDATE_KEY_ID = 2,
  VALIDATE_KEY_TYPE = 4,
  VALIDATE_KEY_LENGTH = 8
};

static uint get_args_count_from_validation_request(int to_validate) {
  uint args_count = 0;

  // Since to_validate is a bit mask - count the number of bits set
  for (; to_validate; to_validate >>= 1)
    if (to_validate & 1) ++args_count;

  return args_count;
}

static bool validate(UDF_ARGS *args, uint expected_arg_count, int to_validate,
                     char *message) {
  THD *thd = current_thd;
  MYSQL_SECURITY_CONTEXT sec_ctx;
  my_svc_bool has_current_user_execute_privilege = 0;

  if (is_keyring_udf_initialized == false) {
    strcpy(message,
           "This function requires keyring_udf plugin which is not installed."
           " Please install keyring_udf plugin and try again.");
    return true;
  }

  if (thd_get_security_context(thd, &sec_ctx) ||
      security_context_get_option(sec_ctx, "privilege_execute",
                                  &has_current_user_execute_privilege))
    return true;

  if (has_current_user_execute_privilege == false) {
    strcpy(message,
           "The user is not privileged to execute this function. "
           "User needs to have EXECUTE permission.");
    return true;
  }

  if (args->arg_count != expected_arg_count) {
    strcpy(message, "Mismatch in number of arguments to the function.");
    return true;
  }

  if (to_validate & VALIDATE_KEY_ID &&
      (args->args[0] == nullptr || args->arg_type[0] != STRING_RESULT)) {
    strcpy(message,
           "Mismatch encountered. A string argument is expected "
           "for key id.");
    return true;
  }

  if (to_validate & VALIDATE_KEY_TYPE &&
      (args->args[1] == nullptr || args->arg_type[1] != STRING_RESULT)) {
    strcpy(message,
           "Mismatch encountered. A string argument is expected "
           "for key type.");
    return true;
  }

  if (to_validate & VALIDATE_KEY_LENGTH) {
    if (args->args[2] == nullptr || args->arg_type[2] != INT_RESULT) {
      strcpy(message,
             "Mismatch encountered. An integer argument is expected "
             "for key length.");
      return true;
    }
    long long key_length = *reinterpret_cast<long long *>(args->args[2]);

    if (key_length > MAX_KEYRING_UDF_KEY_TEXT_LENGTH) {
      sprintf(message, "%s%d",
              "The key is to long. The max length of the key is ",
              MAX_KEYRING_UDF_KEY_TEXT_LENGTH);
      return true;
    }
  }

  if (to_validate & VALIDATE_KEY &&
      (args->args[2] == nullptr || args->arg_type[2] != STRING_RESULT)) {
    strcpy(message,
           "Mismatch encountered. A string argument is expected "
           "for key.");
    return true;
  }
  return false;
}

static bool keyring_udf_func_init(
    UDF_INIT *initid, UDF_ARGS *args, char *message, int to_validate,
    const boost::optional<size_t> max_lenth_to_return,
    const size_t size_of_memory_to_allocate) {
  initid->ptr = nullptr;
  uint expected_arg_count = get_args_count_from_validation_request(to_validate);

  if (validate(args, expected_arg_count, to_validate, message)) return true;

  if (max_lenth_to_return)
    initid->max_length = *max_lenth_to_return;  // if no max_length_to_return
                                                // passed to the function  it
                                                // means that max_length stays
                                                // default
  initid->maybe_null = true;

  if (size_of_memory_to_allocate != 0) {
    initid->ptr = new (std::nothrow) char[size_of_memory_to_allocate];
    if (initid->ptr == nullptr)
      return true;
    else
      memset(initid->ptr, 0, size_of_memory_to_allocate);
  }

  for (uint index = 0; index < expected_arg_count; index++) {
    udf_metadata_service->argument_set(args, type, index,
                                       static_cast<void *>(charset));
  }

  return false;
}

PLUGIN_EXPORT
bool keyring_key_store_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return keyring_udf_func_init(
      initid, args, message,
      (VALIDATE_KEY_ID | VALIDATE_KEY_TYPE | VALIDATE_KEY), 1, 0);
}

PLUGIN_EXPORT
void keyring_key_store_deinit(UDF_INIT *) {}

/**
  Implementation of the UDF:
  INT keyring_key_store(STRING key_id, STRING key_type, STRING key)
  @return 1 on success, NULL and error on failure
*/
PLUGIN_EXPORT
long long keyring_key_store(UDF_INIT *, UDF_ARGS *args, unsigned char *,
                            unsigned char *error) {
  std::string current_user;

  if (get_current_user(&current_user)) {
    *error = 1;
    return 0;
  }

  if (strlen(args->args[2]) > MAX_KEYRING_UDF_KEY_TEXT_LENGTH) {
    my_error(ER_CLIENT_KEYRING_UDF_KEY_TOO_LONG, MYF(0), "keyring_key_store");
    *error = 1;
    return 0;
  }

  if (my_key_store(args->args[0], args->args[1], current_user.c_str(),
                   args->args[2], strlen(args->args[2]))) {
    my_error(ER_KEYRING_UDF_KEYRING_SERVICE_ERROR, MYF(0), "keyring_key_store");
    *error = 1;
    return 0;
  }

  // For the UDF 1 == success, 0 == failure.
  return 1;
}

static bool fetch(const char *function_name, char *key_id, char **a_key,
                  char **a_key_type, size_t *a_key_len) {
  std::string current_user;
  if (get_current_user(&current_user)) return true;

  char *key_type = nullptr, *key = nullptr;
  size_t key_len = 0;

  if (my_key_fetch(key_id, &key_type, current_user.c_str(),
                   reinterpret_cast<void **>(&key), &key_len)) {
    my_error(ER_KEYRING_UDF_KEYRING_SERVICE_ERROR, MYF(0), function_name);

    if (key != nullptr) my_free(key);
    if (key_type != nullptr) my_free(key_type);
    return true;
  }

  if (key == nullptr && key_len > 0) {
    my_error(ER_CLIENT_KEYRING_UDF_KEY_INVALID, MYF(0), function_name);
    if (key_type != nullptr) my_free(key_type);
    return true;
  }

  if (key_len > MAX_KEYRING_UDF_KEY_TEXT_LENGTH) {
    my_error(ER_CLIENT_KEYRING_UDF_KEY_TOO_LONG, MYF(0), function_name);
    if (key != nullptr) my_free(key);
    if (key_type != nullptr) my_free(key_type);
    return true;
  }

  if (key_len != 0) {
    if (key_type == nullptr) {
      my_error(ER_CLIENT_KEYRING_UDF_KEY_TYPE_INVALID, MYF(0), function_name);
      if (key != nullptr) my_free(key);
      return true;
    }

    if (strlen(key_type) > KEYRING_UDF_KEY_TYPE_LENGTH) {
      my_error(ER_CLIENT_KEYRING_UDF_KEY_TYPE_TOO_LONG, MYF(0), function_name);
      if (key != nullptr) my_free(key);
      if (key_type != nullptr) my_free(key_type);
      return true;
    }
  }

  if (a_key != nullptr)
    *a_key = key;
  else
    my_free(key);

  if (a_key_type != nullptr)
    *a_key_type = key_type;
  else
    my_free(key_type);

  if (a_key_len != nullptr) *a_key_len = key_len;

  return false;
}

PLUGIN_EXPORT
bool keyring_key_fetch_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return keyring_udf_func_init(initid, args, message, VALIDATE_KEY_ID,
                               MAX_KEYRING_UDF_KEY_TEXT_LENGTH,
                               MAX_KEYRING_UDF_KEY_TEXT_LENGTH);
}

PLUGIN_EXPORT
void keyring_key_fetch_deinit(UDF_INIT *initid) {
  if (initid->ptr) {
    delete[] initid->ptr;
    initid->ptr = nullptr;
  }
}

/**
  Implementation of the UDF:
  STRING keyring_key_fetch(STRING key_id)
  @return key on success, NULL if key does not exist, NULL and error on failure
*/
PLUGIN_EXPORT
char *keyring_key_fetch(UDF_INIT *initid, UDF_ARGS *args, char *,
                        unsigned long *length, unsigned char *is_null,
                        unsigned char *error) {
  char *key = nullptr;
  size_t key_len = 0;

  if (fetch("keyring_key_fetch", args->args[0], &key, nullptr, &key_len)) {
    if (key != nullptr) my_free(key);
    *error = 1;
    return nullptr;
  }

  if (key != nullptr) {
    memcpy(initid->ptr, key, key_len);
    my_free(key);
  } else
    *is_null = 1;

  *length = key_len;
  *error = 0;
  return initid->ptr;
}

PLUGIN_EXPORT
bool keyring_key_type_fetch_init(UDF_INIT *initid, UDF_ARGS *args,
                                 char *message) {
  return (keyring_udf_func_init(initid, args, message, VALIDATE_KEY_ID,
                                KEYRING_UDF_KEY_TYPE_LENGTH,
                                KEYRING_UDF_KEY_TYPE_LENGTH) ||
          udf_metadata_service->result_set(initid, type,
                                           static_cast<void *>(charset)));
}

PLUGIN_EXPORT
void keyring_key_type_fetch_deinit(UDF_INIT *initid) {
  if (initid->ptr) {
    delete[] initid->ptr;
    initid->ptr = nullptr;
  }
}

/**
  Implementation of the UDF:
  STRING keyring_key_type_fetch(STRING key_id)
  @return key's type on success, NULL if key does not exist, NULL and error on
  failure
*/
PLUGIN_EXPORT
char *keyring_key_type_fetch(UDF_INIT *initid, UDF_ARGS *args, char *,
                             unsigned long *length, unsigned char *is_null,
                             unsigned char *error) {
  char *key_type = nullptr;
  if (fetch("keyring_key_type_fetch", args->args[0], nullptr, &key_type,
            nullptr)) {
    if (key_type != nullptr) my_free(key_type);
    *error = 1;
    return nullptr;
  }

  if (key_type != nullptr) {
    memcpy(initid->ptr, key_type,
           std::min(strlen(key_type), KEYRING_UDF_KEY_TYPE_LENGTH));
    *length = std::min(strlen(key_type), KEYRING_UDF_KEY_TYPE_LENGTH);
    my_free(key_type);
  } else {
    *is_null = 1;
    *length = 0;
  }

  *error = 0;
  return initid->ptr;
}

PLUGIN_EXPORT
bool keyring_key_length_fetch_init(UDF_INIT *initid, UDF_ARGS *args,
                                   char *message) {
  return keyring_udf_func_init(initid, args, message, VALIDATE_KEY_ID,
                               boost::none, 0);
}

PLUGIN_EXPORT
void keyring_key_length_fetch_deinit(UDF_INIT *initid) {
  if (initid->ptr) {
    delete[] initid->ptr;
    initid->ptr = nullptr;
  }
}

/**
  Implementation of the UDF:
  INT keyring_key_length_fetch(STRING key_id)
  @return key's length on success, NULL if key does not exist, NULL and error on
  failure
*/
PLUGIN_EXPORT
long long keyring_key_length_fetch(UDF_INIT *, UDF_ARGS *args,
                                   unsigned char *is_null,
                                   unsigned char *error) {
  size_t key_len = 0;
  char *key = nullptr;

  *error =
      fetch("keyring_key_length_fetch", args->args[0], &key, nullptr, &key_len);

  if (*error == 0 && key == nullptr) *is_null = 1;

  if (key != nullptr) my_free(key);

  // For the UDF 0 == failure.
  return (*error) ? 0 : key_len;
}

PLUGIN_EXPORT
bool keyring_key_remove_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return keyring_udf_func_init(initid, args, message, VALIDATE_KEY_ID, 1, 0);
}

PLUGIN_EXPORT
void keyring_key_remove_deinit(UDF_INIT *) {}

/**
  Implementation of the UDF:
  INT keyring_key_remove(STRING key_id)
  @return 1 on success, NULL on failure
*/
PLUGIN_EXPORT
long long keyring_key_remove(UDF_INIT *, UDF_ARGS *args, unsigned char *,
                             unsigned char *error) {
  std::string current_user;
  if (get_current_user(&current_user)) {
    *error = 1;
    return 0;
  }
  if (my_key_remove(args->args[0], current_user.c_str())) {
    my_error(ER_KEYRING_UDF_KEYRING_SERVICE_ERROR, MYF(0),
             "keyring_key_remove");
    *error = 1;
    return 0;
  }
  *error = 0;
  return 1;
}

PLUGIN_EXPORT
bool keyring_key_generate_init(UDF_INIT *initid, UDF_ARGS *args,
                               char *message) {
  return keyring_udf_func_init(
      initid, args, message,
      (VALIDATE_KEY_ID | VALIDATE_KEY_TYPE | VALIDATE_KEY_LENGTH), 1, 0);
}

PLUGIN_EXPORT
void keyring_key_generate_deinit(UDF_INIT *) {}

/**
  Implementation of the UDF:
  STRING keyring_key_generate(STRING key_id, STRING key_type, INTEGER
  key_length)
  @return 1 on success, NULL and error on failure
*/
PLUGIN_EXPORT
long long keyring_key_generate(UDF_INIT *, UDF_ARGS *args, unsigned char *,
                               unsigned char *error) {
  std::string current_user;
  if (get_current_user(&current_user)) return 0;

  long long key_length = *reinterpret_cast<long long *>(args->args[2]);

  if (my_key_generate(args->args[0], args->args[1], current_user.c_str(),
                      key_length)) {
    my_error(ER_KEYRING_UDF_KEYRING_SERVICE_ERROR, MYF(0),
             "keyring_key_generate");
    *error = 1;
    // For the UDF 1 == success, 0 == failure.
    return 0;
  }
  return 1;
}
