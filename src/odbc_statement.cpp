/*
  Copyright (c) 2013, Dan VerWeire<dverweire@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <time.h>
#include <uv.h>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_result.h"
#include "odbc_statement.h"

using namespace v8;
using namespace node;

Persistent<FunctionTemplate> ODBCStatement::constructor_template;

void ODBCStatement::Init(v8::Handle<Object> exports) {
  DEBUG_PRINTF("ODBCStatement::Init\n");
  NanScope();

  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  // Constructor Template
  NanAssignPersistent(constructor_template, t);
  constructor_template->SetClassName(NanNew("ODBCStatement"));

  // Reserve space for one Handle<Value>
  Local<ObjectTemplate> instance_template = constructor_template->InstanceTemplate();
  instance_template->SetInternalFieldCount(1);
  
  // Prototype Methods
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "execute", Execute);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeSync", ExecuteSync);
  
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeDirect", ExecuteDirect);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeDirectSync", ExecuteDirectSync);
  
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeNonQuery", ExecuteNonQuery);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "executeNonQuerySync", ExecuteNonQuerySync);
  
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "prepare", Prepare);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "prepareSync", PrepareSync);
  
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "bind", Bind);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "bindSync", BindSync);
  
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "closeSync", CloseSync);

  // Attach the Database Constructor to the target object
  exports->Set(NanNew("ODBCStatement"),
               constructor_template->GetFunction());
}

ODBCStatement::~ODBCStatement() {
  this->Free();
}

void ODBCStatement::Free() {
  DEBUG_PRINTF("ODBCStatement::Free\n");
  //if we previously had parameters, then be sure to free them
  if (paramCount) {
    int count = paramCount;
    paramCount = 0;
    
    Parameter prm;
    
    //free parameter memory
    for (int i = 0; i < count; i++) {
      if (prm = params[i], prm.ParameterValuePtr != NULL) {
        switch (prm.ValueType) {
          case SQL_C_WCHAR:   free(prm.ParameterValuePtr);             break;
          case SQL_C_CHAR:    free(prm.ParameterValuePtr);             break; 
          case SQL_C_SBIGINT: delete (int64_t *)prm.ParameterValuePtr; break;
          case SQL_C_DOUBLE:  delete (double  *)prm.ParameterValuePtr; break;
          case SQL_C_BIT:     delete (bool    *)prm.ParameterValuePtr; break;
        }
      }
    }

    free(params);
  }
  
  if (m_hSTMT) {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeHandle(SQL_HANDLE_STMT, m_hSTMT);
    m_hSTMT = NULL;
    
    uv_mutex_unlock(&ODBC::g_odbcMutex);
    
    if (bufferLength > 0) {
      free(buffer);
    }
  }
}

NAN_METHOD(ODBCStatement::New) {
  DEBUG_PRINTF("ODBCStatement::New\n");
  NanScope();
  
  REQ_EXT_ARG(0, js_henv);
  REQ_EXT_ARG(1, js_hdbc);
  REQ_EXT_ARG(2, js_hstmt);
  
  HENV hENV = static_cast<HENV>(js_henv->Value());
  HDBC hDBC = static_cast<HDBC>(js_hdbc->Value());
  HSTMT hSTMT = static_cast<HSTMT>(js_hstmt->Value());
  
  //create a new OBCResult object
  ODBCStatement* stmt = new ODBCStatement(hENV, hDBC, hSTMT);
  
  //specify the buffer length
  stmt->bufferLength = MAX_VALUE_SIZE - 1;
  
  //initialze a buffer for this object
  stmt->buffer = (uint16_t *) malloc(stmt->bufferLength + 1);
  //TODO: make sure the malloc succeeded

  //set the initial colCount to 0
  stmt->colCount = 0;
  
  //initialize the paramCount
  stmt->paramCount = 0;
  
  stmt->Wrap(args.Holder());
  
  NanReturnValue(args.Holder());
}

/*
 * Execute
 */

NAN_METHOD(ODBCStatement::Execute) {
  DEBUG_PRINTF("ODBCStatement::Execute\n");
  
  NanScope();

  REQ_FUN_ARG(0, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  execute_work_data* data = 
    (execute_work_data *) calloc(1, sizeof(execute_work_data));

  NanAssignPersistent(data->cb, cb);
  
  data->stmt = stmt;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(),
    work_req,
    UV_Execute,
    (uv_after_work_cb)UV_AfterExecute);

  stmt->Ref();

  NanReturnUndefined();
}

void ODBCStatement::UV_Execute(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Execute\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLExecute(data->stmt->m_hSTMT); 

  data->result = ret;
}

void ODBCStatement::UV_AfterExecute(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterExecute\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);
  
  NanScope();
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      SQL_HANDLE_STMT,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[4];
    bool* canFreeHandle = new bool(false);
    
    args[0] = External::New(self->m_hENV);
    args[1] = External::New(self->m_hDBC);
    args[2] = External::New(self->m_hSTMT);
    args[3] = External::New(canFreeHandle);
    
    Persistent<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(4, args));

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Object>::New(js_result);

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  self->Unref();
  data->cb.Dispose();
  
  free(data);
  free(req);
}

/*
 * ExecuteSync
 * 
 */

NAN_METHOD(ODBCStatement::ExecuteSync) {
  DEBUG_PRINTF("ODBCStatement::ExecuteSync\n");
  
  NanScope();

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());

  SQLRETURN ret = SQLExecute(stmt->m_hSTMT); 
  
  if(ret == SQL_ERROR) {
    NanThrowError(ODBC::GetSQLError(
      SQL_HANDLE_STMT,
      stmt->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCStatement::ExecuteSync"
    ));
    
    NanReturnNull();
  }
  else {
    Local<Value> args[4];
    bool* canFreeHandle = new bool(false);
    
    args[0] = External::New(stmt->m_hENV);
    args[1] = External::New(stmt->m_hDBC);
    args[2] = External::New(stmt->m_hSTMT);
    args[3] = External::New(canFreeHandle);
    
    Local<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(4, args));
    
    NanReturnValue(js_result);
  }
}

/*
 * ExecuteNonQuery
 */

NAN_METHOD(ODBCStatement::ExecuteNonQuery) {
  DEBUG_PRINTF("ODBCStatement::ExecuteNonQuery\n");
  
  NanScope();

  REQ_FUN_ARG(0, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  execute_work_data* data = 
    (execute_work_data *) calloc(1, sizeof(execute_work_data));

  NanAssignPersistent(data->cb, cb);
  
  data->stmt = stmt;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(),
    work_req,
    UV_ExecuteNonQuery,
    (uv_after_work_cb)UV_AfterExecuteNonQuery);

  stmt->Ref();

  NanReturnUndefined();
}

void ODBCStatement::UV_ExecuteNonQuery(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::ExecuteNonQuery\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLExecute(data->stmt->m_hSTMT); 

  data->result = ret;
}

void ODBCStatement::UV_AfterExecuteNonQuery(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::ExecuteNonQuery\n");
  
  execute_work_data* data = (execute_work_data *)(req->data);
  
  NanScope();
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      SQL_HANDLE_STMT,
      self->m_hSTMT,
      data->cb);
  }
  else {
    SQLLEN rowCount = 0;
    
    SQLRETURN ret = SQLRowCount(self->m_hSTMT, &rowCount);
    
    if (!SQL_SUCCEEDED(ret)) {
      rowCount = 0;
    }
    
    uv_mutex_lock(&ODBC::g_odbcMutex);
    SQLFreeStmt(self->m_hSTMT, SQL_CLOSE);
    uv_mutex_unlock(&ODBC::g_odbcMutex);
    
    Local<Value> args[2];

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Value>::New(Number::New(rowCount));

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  self->Unref();
  data->cb.Dispose();
  
  free(data);
  free(req);
}

/*
 * ExecuteNonQuerySync
 * 
 */

NAN_METHOD(ODBCStatement::ExecuteNonQuerySync) {
  DEBUG_PRINTF("ODBCStatement::ExecuteNonQuerySync\n");
  
  NanScope();

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());

  SQLRETURN ret = SQLExecute(stmt->m_hSTMT); 
  
  if(ret == SQL_ERROR) {
    NanThrowError(ODBC::GetSQLError(
      SQL_HANDLE_STMT,
      stmt->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCStatement::ExecuteSync"
    ));
    
    NanReturnNull();
  }
  else {
    SQLLEN rowCount = 0;
    
    SQLRETURN ret = SQLRowCount(stmt->m_hSTMT, &rowCount);
    
    if (!SQL_SUCCEEDED(ret)) {
      rowCount = 0;
    }
    
    uv_mutex_lock(&ODBC::g_odbcMutex);
    SQLFreeStmt(stmt->m_hSTMT, SQL_CLOSE);
    uv_mutex_unlock(&ODBC::g_odbcMutex);
    
    NanReturnValue(Number::New(rowCount));
  }
}

/*
 * ExecuteDirect
 * 
 */

NAN_METHOD(ODBCStatement::ExecuteDirect) {
  DEBUG_PRINTF("ODBCStatement::ExecuteDirect\n");
  
  NanScope();

  REQ_STRO_ARG(0, sql);
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  execute_direct_work_data* data = 
    (execute_direct_work_data *) calloc(1, sizeof(execute_direct_work_data));

  NanAssignPersistent(data->cb, cb);

  data->sqlLen = sql->Length();

#ifdef UNICODE
  data->sql = (uint16_t *) malloc((data->sqlLen * sizeof(uint16_t)) + sizeof(uint16_t));
  sql->Write((uint16_t *) data->sql);
#else
  data->sql = (char *) malloc(data->sqlLen +1);
  sql->WriteUtf8((char *) data->sql);
#endif

  data->stmt = stmt;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(),
    work_req, 
    UV_ExecuteDirect, 
    (uv_after_work_cb)UV_AfterExecuteDirect);

  stmt->Ref();

  NanReturnUndefined();
}

void ODBCStatement::UV_ExecuteDirect(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_ExecuteDirect\n");
  
  execute_direct_work_data* data = (execute_direct_work_data *)(req->data);

  SQLRETURN ret;
  
  ret = SQLExecDirect(
    data->stmt->m_hSTMT,
    (SQLTCHAR *) data->sql, 
    data->sqlLen);  

  data->result = ret;
}

void ODBCStatement::UV_AfterExecuteDirect(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterExecuteDirect\n");
  
  execute_direct_work_data* data = (execute_direct_work_data *)(req->data);
  
  NanScope();
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      SQL_HANDLE_STMT,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[4];
    bool* canFreeHandle = new bool(false);
    
    args[0] = External::New(self->m_hENV);
    args[1] = External::New(self->m_hDBC);
    args[2] = External::New(self->m_hSTMT);
    args[3] = External::New(canFreeHandle);
    
    Persistent<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(4, args));

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Object>::New(js_result);

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  self->Unref();
  data->cb.Dispose();
  
  free(data->sql);
  free(data);
  free(req);
}

/*
 * ExecuteDirectSync
 * 
 */

NAN_METHOD(ODBCStatement::ExecuteDirectSync) {
  DEBUG_PRINTF("ODBCStatement::ExecuteDirectSync\n");
  
  NanScope();

#ifdef UNICODE
  REQ_WSTR_ARG(0, sql);
#else
  REQ_STR_ARG(0, sql);
#endif

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  SQLRETURN ret = SQLExecDirect(
    stmt->m_hSTMT,
    (SQLTCHAR *) *sql, 
    sql.length());  

  if(ret == SQL_ERROR) {
    NanThrowError(ODBC::GetSQLError(
      SQL_HANDLE_STMT,
      stmt->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCStatement::ExecuteDirectSync"
    ));
    
    NanReturnValue(Null());
  }
  else {
    Local<Value> args[4];
    bool* canFreeHandle = new bool(false);
    
    args[0] = External::New(stmt->m_hENV);
    args[1] = External::New(stmt->m_hDBC);
    args[2] = External::New(stmt->m_hSTMT);
    args[3] = External::New(canFreeHandle);
    
    Persistent<Object> js_result(ODBCResult::constructor_template->
                              GetFunction()->NewInstance(4, args));
    
    NanReturnValue(js_result);
  }
}

/*
 * PrepareSync
 * 
 */

NAN_METHOD(ODBCStatement::PrepareSync) {
  DEBUG_PRINTF("ODBCStatement::PrepareSync\n");
  
  NanScope();

  REQ_STRO_ARG(0, sql);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());

  SQLRETURN ret;

  int sqlLen = sql->Length() + 1;

#ifdef UNICODE
  uint16_t *sql2;
  sql2 = (uint16_t *) malloc(sqlLen * sizeof(uint16_t));
  sql->Write(sql2);
#else
  char *sql2;
  sql2 = (char *) malloc(sqlLen);
  sql->WriteUtf8(sql2);
#endif
  
  ret = SQLPrepare(
    stmt->m_hSTMT,
    (SQLTCHAR *) sql2, 
    sqlLen);
  
  if (SQL_SUCCEEDED(ret)) {
    NanReturnValue(NanTrue());
  }
  else {
    NanThrowError(ODBC::GetSQLError(
      SQL_HANDLE_STMT,
      stmt->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCStatement::PrepareSync"
    ));

    NanReturnValue(NanFalse());
  }
}

/*
 * Prepare
 * 
 */

NAN_METHOD(ODBCStatement::Prepare) {
  DEBUG_PRINTF("ODBCStatement::Prepare\n");
  
  NanScope();

  REQ_STRO_ARG(0, sql);
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  prepare_work_data* data = 
    (prepare_work_data *) calloc(1, sizeof(prepare_work_data));

  NanAssignPersistent(data->cb, cb);

  data->sqlLen = sql->Length();

#ifdef UNICODE
  data->sql = (uint16_t *) malloc((data->sqlLen * sizeof(uint16_t)) + sizeof(uint16_t));
  sql->Write((uint16_t *) data->sql);
#else
  data->sql = (char *) malloc(data->sqlLen +1);
  sql->WriteUtf8((char *) data->sql);
#endif
  
  data->stmt = stmt;
  
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_Prepare, 
    (uv_after_work_cb)UV_AfterPrepare);

  stmt->Ref();

  NanReturnUndefined();
}

void ODBCStatement::UV_Prepare(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Prepare\n");
  
  prepare_work_data* data = (prepare_work_data *)(req->data);

  DEBUG_PRINTF("ODBCStatement::UV_Prepare m_hDBC=%X m_hDBC=%X m_hSTMT=%X\n",
    data->stmt->m_hENV,
    data->stmt->m_hDBC,
    data->stmt->m_hSTMT
  );
  
  SQLRETURN ret;
  
  ret = SQLPrepare(
    data->stmt->m_hSTMT,
    (SQLTCHAR *) data->sql, 
    data->sqlLen);

  data->result = ret;
}

void ODBCStatement::UV_AfterPrepare(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterPrepare\n");
  
  prepare_work_data* data = (prepare_work_data *)(req->data);
  
  DEBUG_PRINTF("ODBCStatement::UV_AfterPrepare m_hDBC=%X m_hDBC=%X m_hSTMT=%X\n",
    data->stmt->m_hENV,
    data->stmt->m_hDBC,
    data->stmt->m_hSTMT
  );
  
  NanScope();

  //First thing, let's check if the execution of the query returned any errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      SQL_HANDLE_STMT,
      data->stmt->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[2];

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Value>::New(NanTrue());

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }
  
  data->stmt->Unref();
  data->cb.Dispose();
  
  free(data->sql);
  free(data);
  free(req);
}

/*
 * BindSync
 * 
 */

NAN_METHOD(ODBCStatement::BindSync) {
  DEBUG_PRINTF("ODBCStatement::BindSync\n");
  
  NanScope();

  if ( !args[0]->IsArray() ) {
    return NanThrowTypeError("Argument 1 must be an Array");
  }

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  DEBUG_PRINTF("ODBCStatement::BindSync m_hDBC=%X m_hDBC=%X m_hSTMT=%X\n",
    stmt->m_hENV,
    stmt->m_hDBC,
    stmt->m_hSTMT
  );
  
  //if we previously had parameters, then be sure to free them
  //before allocating more
  if (stmt->paramCount) {
    int count = stmt->paramCount;
    stmt->paramCount = 0;
    
    Parameter prm;
    
    //free parameter memory
    for (int i = 0; i < count; i++) {
      if (prm = stmt->params[i], prm.ParameterValuePtr != NULL) {
        switch (prm.ValueType) {
          case SQL_C_WCHAR:   free(prm.ParameterValuePtr);             break;
          case SQL_C_CHAR:    free(prm.ParameterValuePtr);             break; 
          case SQL_C_SBIGINT: delete (int64_t *)prm.ParameterValuePtr; break;
          case SQL_C_DOUBLE:  delete (double  *)prm.ParameterValuePtr; break;
          case SQL_C_BIT:     delete (bool    *)prm.ParameterValuePtr; break;
        }
      }
    }

    free(stmt->params);
  }
  
  stmt->params = ODBC::GetParametersFromArray(
    Local<Array>::Cast(args[0]), 
    &stmt->paramCount);
  
  SQLRETURN ret = SQL_SUCCESS;
  Parameter prm;
  
  for (int i = 0; i < stmt->paramCount; i++) {
    prm = stmt->params[i];
    
    DEBUG_PRINTF(
      "ODBCStatement::BindSync - param[%i]: c_type=%i type=%i "
      "buffer_length=%i size=%i length=%i &length=%X decimals=%i value=%s\n",
      i, prm.ValueType, prm.ParameterType, prm.BufferLength, prm.ColumnSize, prm.length, 
      &stmt->params[i].StrLen_or_IndPtr, prm.DecimalDigits, prm.ParameterValuePtr
    );

    ret = SQLBindParameter(
      stmt->m_hSTMT,      //StatementHandle
      i + 1,              //ParameterNumber
      SQL_PARAM_INPUT,    //InputOutputType
      prm.ValueType,
      prm.ParameterType,
      prm.ColumnSize,
      prm.DecimalDigits,
      prm.ParameterValuePtr,
      prm.BufferLength,
      &stmt->params[i].StrLen_or_IndPtr);

    if (ret == SQL_ERROR) {
      break;
    }
  }

  if (SQL_SUCCEEDED(ret)) {
    NanReturnValue(NanTrue());
  }
  else {
    NanThrowError(ODBC::GetSQLError(
      SQL_HANDLE_STMT,
      stmt->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCStatement::BindSync"
    ));
    
    NanReturnValue(NanFalse());
  }

  NanReturnUndefined();
}

/*
 * Bind
 * 
 */

NAN_METHOD(ODBCStatement::Bind) {
  DEBUG_PRINTF("ODBCStatement::Bind\n");
  
  NanScope();

  if ( !args[0]->IsArray() ) {
    return NanThrowError("Argument 1 must be an Array");
  }
  
  REQ_FUN_ARG(1, cb);

  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  bind_work_data* data = 
    (bind_work_data *) calloc(1, sizeof(bind_work_data));

  //if we previously had parameters, then be sure to free them
  //before allocating more
  if (stmt->paramCount) {
    int count = stmt->paramCount;
    stmt->paramCount = 0;
    
    Parameter prm;
    
    //free parameter memory
    for (int i = 0; i < count; i++) {
      if (prm = stmt->params[i], prm.ParameterValuePtr != NULL) {
        switch (prm.ValueType) {
          case SQL_C_WCHAR:   free(prm.ParameterValuePtr);             break;
          case SQL_C_CHAR:    free(prm.ParameterValuePtr);             break; 
          case SQL_C_SBIGINT: delete (int64_t *)prm.ParameterValuePtr; break;
          case SQL_C_DOUBLE:  delete (double  *)prm.ParameterValuePtr; break;
          case SQL_C_BIT:     delete (bool    *)prm.ParameterValuePtr; break;
        }
      }
    }

    free(stmt->params);
  }
  
  data->stmt = stmt;
  
  DEBUG_PRINTF("ODBCStatement::Bind m_hDBC=%X m_hDBC=%X m_hSTMT=%X\n",
    data->stmt->m_hENV,
    data->stmt->m_hDBC,
    data->stmt->m_hSTMT
  );
  
  NanAssignPersistent(data->cb, cb);
  
  data->stmt->params = ODBC::GetParametersFromArray(
    Local<Array>::Cast(args[0]), 
    &data->stmt->paramCount);
  
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_Bind, 
    (uv_after_work_cb)UV_AfterBind);

  stmt->Ref();

  NanReturnUndefined();
}

void ODBCStatement::UV_Bind(uv_work_t* req) {
  DEBUG_PRINTF("ODBCStatement::UV_Bind\n");
  
  bind_work_data* data = (bind_work_data *)(req->data);

  DEBUG_PRINTF("ODBCStatement::UV_Bind m_hDBC=%X m_hDBC=%X m_hSTMT=%X\n",
    data->stmt->m_hENV,
    data->stmt->m_hDBC,
    data->stmt->m_hSTMT
  );
  
  SQLRETURN ret = SQL_SUCCESS;
  Parameter prm;
  
  for (int i = 0; i < data->stmt->paramCount; i++) {
    prm = data->stmt->params[i];
    
    DEBUG_PRINTF(
      "ODBCStatement::UV_Bind - param[%i]: c_type=%i type=%i "
      "buffer_length=%i size=%i length=%i &length=%X decimals=%i value=%s\n",
      i, prm.ValueType, prm.ParameterType, prm.BufferLength, prm.ColumnSize, prm.length, 
      &data->stmt->params[i].StrLen_or_IndPtr, prm.DecimalDigits, prm.ParameterValuePtr
    );

    ret = SQLBindParameter(
      data->stmt->m_hSTMT,        //StatementHandle
      i + 1,                      //ParameterNumber
      SQL_PARAM_INPUT,            //InputOutputType
      prm.ValueType,
      prm.ParameterType,
      prm.ColumnSize,
      prm.DecimalDigits,
      prm.ParameterValuePtr,
      prm.BufferLength,
      &data->stmt->params[i].StrLen_or_IndPtr);

    if (ret == SQL_ERROR) {
      break;
    }
  }

  data->result = ret;
}

void ODBCStatement::UV_AfterBind(uv_work_t* req, int status) {
  DEBUG_PRINTF("ODBCStatement::UV_AfterBind\n");
  
  bind_work_data* data = (bind_work_data *)(req->data);
  
  NanScope();
  
  //an easy reference to the statment object
  ODBCStatement* self = data->stmt->self();

  //Check if there were errors 
  if(data->result == SQL_ERROR) {
    ODBC::CallbackSQLError(
      SQL_HANDLE_STMT,
      self->m_hSTMT,
      data->cb);
  }
  else {
    Local<Value> args[2];

    args[0] = Local<Value>::New(Null());
    args[1] = Local<Value>::New(NanTrue());

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }

  self->Unref();
  data->cb.Dispose();
  
  free(data);
  free(req);
}

/*
 * CloseSync
 */

NAN_METHOD(ODBCStatement::CloseSync) {
  DEBUG_PRINTF("ODBCStatement::CloseSync\n");
  
  NanScope();

  OPT_INT_ARG(0, closeOption, SQL_DESTROY);
  
  ODBCStatement* stmt = ObjectWrap::Unwrap<ODBCStatement>(args.Holder());
  
  DEBUG_PRINTF("ODBCStatement::CloseSync closeOption=%i\n", 
               closeOption);
  
  if (closeOption == SQL_DESTROY) {
    stmt->Free();
  }
  else {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeStmt(stmt->m_hSTMT, closeOption);
  
    uv_mutex_unlock(&ODBC::g_odbcMutex);
  }

  NanReturnValue(NanTrue());
}
