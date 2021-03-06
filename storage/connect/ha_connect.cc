/* Copyright (C) Olivier Bertrand 2004 - 2014

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/**
  @file ha_connect.cc

  @brief
  The ha_connect engine is a stubbed storage engine that enables to create tables
  based on external data. Principally they are based on plain files of many
  different types, but also on collections of such files, collection of tables,
  local or remote MySQL/MariaDB tables retrieved via MySQL API,
  ODBC tables retrieving data from other DBMS having an ODBC server, and even
  virtual tables.

  @details
  ha_connect will let you create/open/delete tables, the created table can be
  done specifying an already existing file, the drop table command will just
  suppress the table definition but not the eventual data file.
  Indexes are not supported for all table types but data can be inserted,
  updated or deleted.

  You can enable the CONNECT storage engine in your build by doing the
  following during your build process:<br> ./configure
  --with-connect-storage-engine

  You can install the CONNECT handler as all other storage handlers.

  Once this is done, MySQL will let you create tables with:<br>
  CREATE TABLE <table name> (...) ENGINE=CONNECT;

  The example storage engine does not use table locks. It
  implements an example "SHARE" that is inserted into a hash by table
  name. This is not used yet.

  Please read the object definition in ha_connect.h before reading the rest
  of this file.

  @note
  This MariaDB CONNECT handler is currently an adaptation of the XDB handler
  that was written for MySQL version 4.1.2-alpha. Its overall design should
  be enhanced in the future to meet MariaDB requirements.

  @note
  It was written also from the Brian's ha_example handler and contains parts
  of it that are there, such as table and system  variables.

  @note
  When you create an CONNECT table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL.
  For file based tables, if a file name is not specified, this is an inward
  table. An empty file is made in the current data directory that you can
  populate later like for other engine tables. This file modified on ALTER
  and is deleted when dropping the table.
  If a file name is specified, this in an outward table. The specified file
  will be used as representing the table data and will not be modified or
  deleted on command such as ALTER or DROP.
  To get an idea of what occurs, here is an example select that would do
  a scan of an entire table:

  @code
  ha-connect::open
  ha_connect::store_lock
  ha_connect::external_lock
  ha_connect::info
  ha_connect::rnd_init
  ha_connect::extra
  ENUM HA_EXTRA_CACHE        Cache record in HA_rrnd()
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::extra
  ENUM HA_EXTRA_NO_CACHE     End caching of records (def)
  ha_connect::external_lock
  ha_connect::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the connect storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Calls to
  ha_connect::extra() are hints as to what will be occuring to the request.

  Happy use!<br>
    -Olivier
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define MYSQL_SERVER 1
#define DONT_DEFINE_VOID
#include "sql_class.h"
#include "create_options.h"
#include "mysql_com.h"
#include "field.h"
#include "sql_parse.h"
#include "sql_base.h"
#include <sys/stat.h>
#if defined(NEW_WAY)
#include "sql_table.h"
#endif   // NEW_WAY
#include "sql_partition.h"
#undef  OFFSET

#define NOPARSE
#if defined(UNIX)
#include "osutil.h"
#endif   // UNIX
#include "global.h"
#include "plgdbsem.h"
#if defined(ODBC_SUPPORT)
#include "odbccat.h"
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
#include "xtable.h"
#include "tabmysql.h"
#endif   // MYSQL_SUPPORT
#include "filamdbf.h"
#include "tabxcl.h"
#include "tabfmt.h"
#include "reldef.h"
#include "tabcol.h"
#include "xindex.h"
#if defined(WIN32)
#include <io.h>
#include "tabwmi.h"
#endif   // WIN32
#include "connect.h"
#include "user_connect.h"
#include "ha_connect.h"
#include "mycat.h"
#include "myutil.h"
#include "preparse.h"
#include "inihandl.h"
#if defined(LIBXML2_SUPPORT)
#include "libdoc.h"
#endif   // LIBXML2_SUPPORT
#include "taboccur.h"
#include "tabpivot.h"

#define my_strupr(p)    my_caseup_str(default_charset_info, (p));
#define my_strlwr(p)    my_casedn_str(default_charset_info, (p));
#define my_stricmp(a,b) my_strcasecmp(default_charset_info, (a), (b))


/***********************************************************************/
/*  Initialize the ha_connect static members.                          */
/***********************************************************************/
#define SZCONV 8192
#define SZWORK 67108864            // Default work area size 64M
#define SZWMIN 4194304             // Minimum work area size  4M

extern "C" {
       char  version[]= "Version 1.03.0003 August 22, 2014";
       char  compver[]= "Version 1.03.0003 " __DATE__ " "  __TIME__;

#if defined(WIN32)
       char slash= '\\';
#else   // !WIN32
       char slash= '/';
#endif  // !WIN32

#if defined(XMSG)
       char  msglang[];            // Default message language
#endif
       int  trace= 0;              // The general trace value
       int  xconv= 0;              // The type conversion option
       int  zconv= SZCONV;         // The text conversion size
       USETEMP Use_Temp= TMP_AUTO; // The temporary file use
} // extern "C"

#if defined(XMAP)
       bool xmap= false;
#endif   // XMAP
       bool xinfo= false;

       uint worksize= SZWORK;
ulong  ha_connect::num= 0;
//int  DTVAL::Shift= 0;

/* CONNECT system variables */
static int     xtrace= 0;
static int     conv_size= SZCONV;
static uint    work_size= SZWORK;
static ulong   type_conv= 0;
static ulong   use_tempfile= 1;
#if defined(XMAP)
static my_bool indx_map= 0;
#endif   // XMAP
static my_bool exact_info= 0;

/***********************************************************************/
/*  Utility functions.                                                 */
/***********************************************************************/
PQRYRES OEMColumns(PGLOBAL g, PTOS topt, char *tab, char *db, bool info);
void PushWarning(PGLOBAL g, THD *thd, int level);
bool CheckSelf(PGLOBAL g, TABLE_SHARE *s, const char *host,
                   const char *db, char *tab, const char *src, int port);


static PCONNECT GetUser(THD *thd, PCONNECT xp);
static PGLOBAL  GetPlug(THD *thd, PCONNECT& lxp);

static handler *connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root);

static int connect_assisted_discovery(handlerton *hton, THD* thd,
                                      TABLE_SHARE *table_s,
                                      HA_CREATE_INFO *info);

/***********************************************************************/
/*  Global variables update functions.                                 */
/***********************************************************************/
static void update_connect_xtrace(MYSQL_THD thd,
                                  struct st_mysql_sys_var *var,
                                  void *var_ptr, const void *save)
{
  trace= *(int *)var_ptr= *(int *)save;
} // end of update_connect_xtrace

static void update_connect_zconv(MYSQL_THD thd,
                                  struct st_mysql_sys_var *var,
                                  void *var_ptr, const void *save)
{
  zconv= *(int *)var_ptr= *(int *)save;
} // end of update_connect_zconv

static void update_connect_xconv(MYSQL_THD thd,
                                 struct st_mysql_sys_var *var,
                                 void *var_ptr, const void *save)
{
  xconv= (int)(*(ulong *)var_ptr= *(ulong *)save);
} // end of update_connect_xconv

static void update_connect_worksize(MYSQL_THD thd,
                                 struct st_mysql_sys_var *var,
                                 void *var_ptr, const void *save)
{
  worksize= (uint)(*(ulong *)var_ptr= *(ulong *)save);
} // end of update_connect_worksize

static void update_connect_usetemp(MYSQL_THD thd,
                                   struct st_mysql_sys_var *var,
                                   void *var_ptr, const void *save)
{
  Use_Temp= (USETEMP)(*(ulong *)var_ptr= *(ulong *)save);
} // end of update_connect_usetemp

#if defined(XMAP)
static void update_connect_xmap(MYSQL_THD thd,
                                struct st_mysql_sys_var *var,
                                void *var_ptr, const void *save)
{
  xmap= (bool)(*(my_bool *)var_ptr= *(my_bool *)save);
} // end of update_connect_xmap
#endif   // XMAP

static void update_connect_xinfo(MYSQL_THD thd,
                                 struct st_mysql_sys_var *var,
                                 void *var_ptr, const void *save)
{
  xinfo= (bool)(*(my_bool *)var_ptr= *(my_bool *)save);
} // end of update_connect_xinfo

/***********************************************************************/
/*  The CONNECT handlerton object.                                     */
/***********************************************************************/
handlerton *connect_hton;

/**
  CREATE TABLE option list (table options)

  These can be specified in the CREATE TABLE:
  CREATE TABLE ( ... ) {...here...}
*/
ha_create_table_option connect_table_option_list[]=
{
  HA_TOPTION_STRING("TABLE_TYPE", type),
  HA_TOPTION_STRING("FILE_NAME", filename),
  HA_TOPTION_STRING("XFILE_NAME", optname),
//HA_TOPTION_STRING("CONNECT_STRING", connect),
  HA_TOPTION_STRING("TABNAME", tabname),
  HA_TOPTION_STRING("TABLE_LIST", tablist),
  HA_TOPTION_STRING("DBNAME", dbname),
  HA_TOPTION_STRING("SEP_CHAR", separator),
  HA_TOPTION_STRING("QCHAR", qchar),
  HA_TOPTION_STRING("MODULE", module),
  HA_TOPTION_STRING("SUBTYPE", subtype),
  HA_TOPTION_STRING("CATFUNC", catfunc),
  HA_TOPTION_STRING("SRCDEF", srcdef),
  HA_TOPTION_STRING("COLIST", colist),
  HA_TOPTION_STRING("OPTION_LIST", oplist),
  HA_TOPTION_STRING("DATA_CHARSET", data_charset),
  HA_TOPTION_NUMBER("LRECL", lrecl, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("BLOCK_SIZE", elements, 0, 0, INT_MAX32, 1),
//HA_TOPTION_NUMBER("ESTIMATE", estimate, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("MULTIPLE", multiple, 0, 0, 2, 1),
  HA_TOPTION_NUMBER("HEADER", header, 0, 0, 3, 1),
  HA_TOPTION_NUMBER("QUOTED", quoted, (ulonglong) -1, 0, 3, 1),
  HA_TOPTION_NUMBER("ENDING", ending, (ulonglong) -1, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("COMPRESS", compressed, 0, 0, 2, 1),
//HA_TOPTION_BOOL("COMPRESS", compressed, 0),
  HA_TOPTION_BOOL("MAPPED", mapped, 0),
  HA_TOPTION_BOOL("HUGE", huge, 0),
  HA_TOPTION_BOOL("SPLIT", split, 0),
  HA_TOPTION_BOOL("READONLY", readonly, 0),
  HA_TOPTION_BOOL("SEPINDEX", sepindex, 0),
  HA_TOPTION_END
};


/**
  CREATE TABLE option list (field options)

  These can be specified in the CREATE TABLE per field:
  CREATE TABLE ( field ... {...here...}, ... )
*/
ha_create_table_option connect_field_option_list[]=
{
  HA_FOPTION_NUMBER("FLAG", offset, (ulonglong) -1, 0, INT_MAX32, 1),
  HA_FOPTION_NUMBER("MAX_DIST", freq, 0, 0, INT_MAX32, 1), // BLK_INDX
//HA_FOPTION_NUMBER("DISTRIB", opt, 0, 0, 2, 1),  // used for BLK_INDX
  HA_FOPTION_NUMBER("FIELD_LENGTH", fldlen, 0, 0, INT_MAX32, 1),
  HA_FOPTION_STRING("DATE_FORMAT", dateformat),
  HA_FOPTION_STRING("FIELD_FORMAT", fieldformat),
  HA_FOPTION_STRING("SPECIAL", special),
  HA_FOPTION_ENUM("DISTRIB", opt, "scattered,clustered,sorted", 0),
  HA_FOPTION_END
};

/*
  CREATE TABLE option list (index options)

  These can be specified in the CREATE TABLE per index:
  CREATE TABLE ( field ..., .., INDEX .... *here*, ... )
*/
ha_create_table_option connect_index_option_list[]=
{
  HA_IOPTION_BOOL("DYNAM", dynamic, 0),
  HA_IOPTION_BOOL("MAPPED", mapped, 0),
  HA_IOPTION_END
};

/***********************************************************************/
/*  Push G->Message as a MySQL warning.                                */
/***********************************************************************/
bool PushWarning(PGLOBAL g, PTDBASE tdbp, int level)
{
  PHC    phc;
  THD   *thd;
  MYCAT *cat= (MYCAT*)tdbp->GetDef()->GetCat();

  if (!cat || !(phc= cat->GetHandler()) || !phc->GetTable() ||
      !(thd= (phc->GetTable())->in_use))
    return true;

  PushWarning(g, thd, level);
  return false;
} // end of PushWarning

void PushWarning(PGLOBAL g, THD *thd, int level)
  {
  if (thd) {
    Sql_condition::enum_warning_level wlvl;

    wlvl= (Sql_condition::enum_warning_level)level;
    push_warning(thd, wlvl, 0, g->Message);
  } else
    htrc("%s\n", g->Message);

  } // end of PushWarning

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key con_key_mutex_CONNECT_SHARE_mutex;

static PSI_mutex_info all_connect_mutexes[]=
{
  { &con_key_mutex_CONNECT_SHARE_mutex, "CONNECT_SHARE::mutex", 0}
};

static void init_connect_psi_keys()
{
  const char* category= "connect";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_connect_mutexes);
  PSI_server->register_mutex(category, all_connect_mutexes, count);
}
#else
static void init_connect_psi_keys() {}
#endif


DllExport LPCSTR PlugSetPath(LPSTR to, LPCSTR name, LPCSTR dir)
{
  const char *res= PlugSetPath(to, mysql_data_home, name, dir);
  return res;
}


/**
  @brief
  If frm_error() is called then we will use this to determine
  the file extensions that exist for the storage engine. This is also
  used by the default rename_table and delete_table method in
  handler.cc.

  For engines that have two file name extentions (separate meta/index file
  and data file), the order of elements is relevant. First element of engine
  file name extentions array should be meta/index file extention. Second
  element - data file extention. This order is assumed by
  prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

  @see
  rename_table method in handler.cc and
  delete_table method in handler.cc
*/
static const char *ha_connect_exts[]= {
  ".dos", ".fix", ".csv", ".bin", ".fmt", ".dbf", ".xml", ".ini", ".vec",
  ".dnx", ".fnx", ".bnx", ".vnx", ".dbx", ".dop", ".fop", ".bop", ".vop",
  NULL};

/**
  @brief
  Plugin initialization
*/
static int connect_init_func(void *p)
{
  DBUG_ENTER("connect_init_func");

  sql_print_information("CONNECT: %s", compver);

  // xtrace is now a system variable
  trace= xtrace;

#ifdef LIBXML2_SUPPORT
  XmlInitParserLib();
#endif   // LIBXML2_SUPPORT

  init_connect_psi_keys();

  connect_hton= (handlerton *)p;
  connect_hton->state= SHOW_OPTION_YES;
  connect_hton->create= connect_create_handler;
//connect_hton->flags= HTON_TEMPORARY_NOT_SUPPORTED | HTON_NO_PARTITION;
  connect_hton->flags= HTON_TEMPORARY_NOT_SUPPORTED;
  connect_hton->table_options= connect_table_option_list;
  connect_hton->field_options= connect_field_option_list;
  connect_hton->index_options= connect_index_option_list;
  connect_hton->tablefile_extensions= ha_connect_exts;
  connect_hton->discover_table_structure= connect_assisted_discovery;

  if (xtrace)
    sql_print_information("connect_init: hton=%p", p);

  DTVAL::SetTimeShift();      // Initialize time zone shift once for all
  DBUG_RETURN(0);
} // end of connect_init_func


/**
  @brief
  Plugin clean up
*/
static int connect_done_func(void *p)
{
  int error= 0;
  PCONNECT pc, pn;
  DBUG_ENTER("connect_done_func");

#ifdef LIBXML2_SUPPORT
  XmlCleanupParserLib();
#endif   // LIBXML2_SUPPORT

#if !defined(WIN32)
//PROFILE_End();                Causes signal 11
#endif   // !WIN32

  for (pc= user_connect::to_users; pc; pc= pn) {
    if (pc->g)
      PlugCleanup(pc->g, true);

    pn= pc->next;
    delete pc;
    } // endfor pc

  DBUG_RETURN(error);
} // end of connect_done_func


/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each CONNECT handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

CONNECT_SHARE *ha_connect::get_share()
{
  CONNECT_SHARE *tmp_share;

  lock_shared_ha_data();

  if (!(tmp_share= static_cast<CONNECT_SHARE*>(get_ha_share_ptr()))) {
    tmp_share= new CONNECT_SHARE;
    if (!tmp_share)
      goto err;
    mysql_mutex_init(con_key_mutex_CONNECT_SHARE_mutex,
                     &tmp_share->mutex, MY_MUTEX_INIT_FAST);
    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
    } // endif tmp_share

 err:
  unlock_shared_ha_data();
  return tmp_share;
} // end of get_share


static handler* connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root)
{
  handler *h= new (mem_root) ha_connect(hton, table);

  if (xtrace)
    htrc("New CONNECT %p, table: %s\n",
                         h, table ? table->table_name.str : "<null>");

  return h;
} // end of connect_create_handler

/****************************************************************************/
/*  ha_connect constructor.                                                 */
/****************************************************************************/
ha_connect::ha_connect(handlerton *hton, TABLE_SHARE *table_arg)
       :handler(hton, table_arg)
{
  hnum= ++num;
  xp= (table) ? GetUser(ha_thd(), NULL) : NULL;
  if (xp)
    xp->SetHandler(this);
#if defined(WIN32)
  datapath= ".\\";
#else   // !WIN32
  datapath= "./";
#endif  // !WIN32
  tdbp= NULL;
  sdvalin= NULL;
  sdvalout= NULL;
  xmod= MODE_ANY;
  istable= false;
  *partname= 0;
  bzero((char*) &xinfo, sizeof(XINFO));
  valid_info= false;
  valid_query_id= 0;
  creat_query_id= (table && table->in_use) ? table->in_use->query_id : 0;
  stop= false;
  alter= false;
  mrr= false;
  nox= true;
  abort= false;
  indexing= -1;
  locked= 0;
  part_id= NULL;
  data_file_name= NULL;
  index_file_name= NULL;
  enable_activate_all_index= 0;
  int_table_flags= (HA_NO_TRANSACTIONS | HA_NO_PREFIX_CHAR_KEYS);
  ref_length= sizeof(int);
  share= NULL;
  tshp= NULL;
} // end of ha_connect constructor


/****************************************************************************/
/*  ha_connect destructor.                                                  */
/****************************************************************************/
ha_connect::~ha_connect(void)
{
  if (xtrace)
    htrc("Delete CONNECT %p, table: %s, xp=%p count=%d\n", this,
                         table ? table->s->table_name.str : "<null>",
                         xp, xp ? xp->count : 0);

  if (xp) {
    PCONNECT p;

    xp->count--;

    for (p= user_connect::to_users; p; p= p->next)
      if (p == xp)
        break;

    if (p && !p->count) {
      if (p->next)
        p->next->previous= p->previous;

      if (p->previous)
        p->previous->next= p->next;
      else
        user_connect::to_users= p->next;

      } // endif p

    if (!xp->count) {
      PlugCleanup(xp->g, true);
      delete xp;
      } // endif count

    } // endif xp

} // end of ha_connect destructor


/****************************************************************************/
/*  Get a pointer to the user of this handler.                              */
/****************************************************************************/
static PCONNECT GetUser(THD *thd, PCONNECT xp)
{
  const char *dbn= NULL;

  if (!thd)
    return NULL;

  if (xp && thd == xp->thdp)
    return xp;

  for (xp= user_connect::to_users; xp; xp= xp->next)
    if (thd == xp->thdp)
      break;

  if (!xp) {
    xp= new user_connect(thd, dbn);

    if (xp->user_init()) {
      delete xp;
      xp= NULL;
      } // endif user_init

  } else
    xp->count++;

  return xp;
} // end of GetUser


/****************************************************************************/
/*  Get the global pointer of the user of this handler.                     */
/****************************************************************************/
static PGLOBAL GetPlug(THD *thd, PCONNECT& lxp)
{
  lxp= GetUser(thd, lxp);
  return (lxp) ? lxp->g : NULL;
} // end of GetPlug

/****************************************************************************/
/*  Get the implied table type.                                             */
/****************************************************************************/
TABTYPE ha_connect::GetRealType(PTOS pos)
{
  TABTYPE type;
  
  if (pos || (pos= GetTableOptionStruct())) {
    type= GetTypeID(pos->type);

    if (type == TAB_UNDEF)
      type= pos->srcdef ? TAB_MYSQL : pos->tabname ? TAB_PRX : TAB_DOS;

  } else
    type= TAB_UNDEF;

  return type;
} // end of GetRealType

/** @brief
  The name of the index type that will be used for display.
  Don't implement this method unless you really have indexes.
 */
const char *ha_connect::index_type(uint inx) 
{ 
  switch (GetIndexType(GetRealType())) {
    case 1:
      if (table_share)
        return (GetIndexOption(&table_share->key_info[inx], "Dynamic"))
             ? "KINDEX" : "XINDEX";
      else
        return "XINDEX";

    case 2: return "REMOTE";
    } // endswitch

  return "Unknown";
} // end of index_type

/** @brief
  This is a bitmap of flags that indicates how the storage engine
  implements indexes. The current index flags are documented in
  handler.h. If you do not implement indexes, just return zero here.

    @details
  part is the key part to check. First key part is 0.
  If all_parts is set, MySQL wants to know the flags for the combined
  index, up to and including 'part'.
*/
ulong ha_connect::index_flags(uint inx, uint part, bool all_parts) const
{
  ulong       flags= HA_READ_NEXT | HA_READ_RANGE |
                     HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
  ha_connect *hp= (ha_connect*)this;
  PTOS        pos= hp->GetTableOptionStruct();

  if (pos) {
    TABTYPE type= hp->GetRealType(pos);

    switch (GetIndexType(type)) {
      case 1: flags|= (HA_READ_ORDER | HA_READ_PREV); break;
      case 2: flags|= HA_READ_AFTER_KEY;              break;
      } // endswitch

    } // endif pos

  return flags;
} // end of index_flags

/** @brief
  This is a list of flags that indicate what functionality the storage
  engine implements. The current table flags are documented in handler.h
*/
ulonglong ha_connect::table_flags() const
{
  ulonglong   flags= HA_CAN_VIRTUAL_COLUMNS | HA_REC_NOT_IN_SEQ |
                     HA_NO_AUTO_INCREMENT | HA_NO_PREFIX_CHAR_KEYS |
                     HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
                     HA_PARTIAL_COLUMN_READ | HA_FILE_BASED |
//                   HA_NULL_IN_KEY |    not implemented yet
//                   HA_FAST_KEY_READ |  causes error when sorting (???)
                     HA_NO_TRANSACTIONS | HA_DUPLICATE_KEY_NOT_IN_ORDER |
                     HA_NO_BLOBS | HA_MUST_USE_TABLE_CONDITION_PUSHDOWN;
  ha_connect *hp= (ha_connect*)this;
  PTOS        pos= hp->GetTableOptionStruct();

  if (pos) {
    TABTYPE type= hp->GetRealType(pos);

    if (IsFileType(type))
      flags|= HA_FILE_BASED;

    if (IsExactType(type))
      flags|= (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT);

    // No data change on ALTER for outward tables
    if (!IsFileType(type) || hp->FileExists(pos->filename, true))
      flags|= HA_NO_COPY_ON_ALTER;

    } // endif pos

  return flags;
} // end of table_flags

/****************************************************************************/
/*  Return the value of an option specified in an option list.              */
/****************************************************************************/
char *GetListOption(PGLOBAL g, const char *opname,
                               const char *oplist, const char *def)
{
  char  key[16], val[256];
  char *pk, *pv, *pn;
  char *opval= (char*) def;
  int   n;

  for (pk= (char*)oplist; pk; pk= ++pn) {
    pn= strchr(pk, ',');
    pv= strchr(pk, '=');

    if (pv && (!pn || pv < pn)) {
      n= pv - pk;
      memcpy(key, pk, n);
      key[n]= 0;
      pv++;

      if (pn) {
        n= pn - pv;
        memcpy(val, pv, n);
        val[n]= 0;
      } else
        strcpy(val, pv);

    } else {
      if (pn) {
        n= MY_MIN(pn - pk, 15);
        memcpy(key, pk, n);
        key[n]= 0;
      } else
        strcpy(key, pk);

      val[0]= 0;
    } // endif pv

    if (!stricmp(opname, key)) {
      opval= (char*)PlugSubAlloc(g, NULL, strlen(val) + 1);
      strcpy(opval, val);
      break;
    } else if (!pn)
      break;

    } // endfor pk

  return opval;
} // end of GetListOption

/****************************************************************************/
/*  Return the table option structure.                                      */
/****************************************************************************/
PTOS ha_connect::GetTableOptionStruct(TABLE_SHARE *s)
{
  TABLE_SHARE *tsp= (tshp) ? tshp : (s) ? s : table_share;

  return (tsp) ? tsp->option_struct : NULL;
} // end of GetTableOptionStruct

/****************************************************************************/
/*  Return the string eventually formatted with partition name.             */
/****************************************************************************/
char *ha_connect::GetRealString(const char *s)
{
  char *sv;

  if (IsPartitioned() && s) {
    sv= (char*)PlugSubAlloc(xp->g, NULL, strlen(s) + strlen(partname));
    sprintf(sv, s, partname);
  } else
    sv= (char*)s;

  return sv;
} // end of GetRealString

/****************************************************************************/
/*  Return the value of a string option or NULL if not specified.           */
/****************************************************************************/
char *ha_connect::GetStringOption(char *opname, char *sdef)
{
  char *opval= NULL;
  PTOS  options= GetTableOptionStruct();

  if (!stricmp(opname, "Connect")) {
    LEX_STRING cnc= (tshp) ? tshp->connect_string : table->s->connect_string;

    if (cnc.length)
      opval= GetRealString(cnc.str);

  } else if (!stricmp(opname, "Query_String"))
    opval= thd_query_string(table->in_use)->str;
  else if (!stricmp(opname, "Partname"))
    opval= partname;
  else if (!options)
    ;
  else if (!stricmp(opname, "Type"))
    opval= (char*)options->type;
  else if (!stricmp(opname, "Filename"))
    opval= GetRealString(options->filename);
  else if (!stricmp(opname, "Optname"))
    opval= (char*)options->optname;
  else if (!stricmp(opname, "Tabname"))
    opval= GetRealString(options->tabname);
  else if (!stricmp(opname, "Tablist"))
    opval= (char*)options->tablist;
  else if (!stricmp(opname, "Database") ||
           !stricmp(opname, "DBname"))
    opval= (char*)options->dbname;
  else if (!stricmp(opname, "Separator"))
    opval= (char*)options->separator;
  else if (!stricmp(opname, "Qchar"))
    opval= (char*)options->qchar;
  else if (!stricmp(opname, "Module"))
    opval= (char*)options->module;
  else if (!stricmp(opname, "Subtype"))
    opval= (char*)options->subtype;
  else if (!stricmp(opname, "Catfunc"))
    opval= (char*)options->catfunc;
  else if (!stricmp(opname, "Srcdef"))
    opval= (char*)options->srcdef;
  else if (!stricmp(opname, "Colist"))
    opval= (char*)options->colist;
  else if (!stricmp(opname, "Data_charset"))
    opval= (char*)options->data_charset;

  if (!opval && options && options->oplist)
    opval= GetListOption(xp->g, opname, options->oplist);

  if (!opval) {
    if (sdef && !strcmp(sdef, "*")) {
      // Return the handler default value
      if (!stricmp(opname, "Dbname") || !stricmp(opname, "Database"))
        opval= (char*)GetDBName(NULL);    // Current database
      else if (!stricmp(opname, "Type"))  // Default type
        opval= (!options) ? NULL :
               (options->srcdef)  ? (char*)"MYSQL" :
               (options->tabname) ? (char*)"PROXY" : (char*)"DOS";
      else if (!stricmp(opname, "User"))  // Connected user
        opval= (char *) "root";
      else if (!stricmp(opname, "Host"))  // Connected user host
        opval= (char *) "localhost";
      else
        opval= sdef;                      // Caller default

    } else
      opval= sdef;                        // Caller default

    } // endif !opval

  return opval;
} // end of GetStringOption

/****************************************************************************/
/*  Return the value of a Boolean option or bdef if not specified.          */
/****************************************************************************/
bool ha_connect::GetBooleanOption(char *opname, bool bdef)
{
  bool  opval= bdef;
  char *pv;
  PTOS  options= GetTableOptionStruct();

  if (!stricmp(opname, "View"))
    opval= (tshp) ? tshp->is_view : table_share->is_view;
  else if (!options)
    ;
  else if (!stricmp(opname, "Mapped"))
    opval= options->mapped;
  else if (!stricmp(opname, "Huge"))
    opval= options->huge;
//else if (!stricmp(opname, "Compressed"))
//  opval= options->compressed;
  else if (!stricmp(opname, "Split"))
    opval= options->split;
  else if (!stricmp(opname, "Readonly"))
    opval= options->readonly;
  else if (!stricmp(opname, "SepIndex"))
    opval= options->sepindex;
  else if (options->oplist)
    if ((pv= GetListOption(xp->g, opname, options->oplist)))
      opval= (!*pv || *pv == 'y' || *pv == 'Y' || atoi(pv) != 0);

  return opval;
} // end of GetBooleanOption

/****************************************************************************/
/*  Set the value of the opname option (does not work for oplist options)   */
/*  Currently used only to set the Sepindex value.                          */
/****************************************************************************/
bool ha_connect::SetBooleanOption(char *opname, bool b)
{
  PTOS options= GetTableOptionStruct();

  if (!options)
    return true;

  if (!stricmp(opname, "SepIndex"))
    options->sepindex= b;
  else
    return true;

  return false;
} // end of SetBooleanOption

/****************************************************************************/
/*  Return the value of an integer option or NO_IVAL if not specified.      */
/****************************************************************************/
int ha_connect::GetIntegerOption(char *opname)
{
  ulonglong    opval= NO_IVAL;
  char        *pv;
  PTOS         options= GetTableOptionStruct();
  TABLE_SHARE *tsp= (tshp) ? tshp : table_share;

  if (!stricmp(opname, "Avglen"))
    opval= (ulonglong)tsp->avg_row_length;
  else if (!stricmp(opname, "Estimate"))
    opval= (ulonglong)tsp->max_rows;
  else if (!options)
    ;
  else if (!stricmp(opname, "Lrecl"))
    opval= options->lrecl;
  else if (!stricmp(opname, "Elements"))
    opval= options->elements;
  else if (!stricmp(opname, "Multiple"))
    opval= options->multiple;
  else if (!stricmp(opname, "Header"))
    opval= options->header;
  else if (!stricmp(opname, "Quoted"))
    opval= options->quoted;
  else if (!stricmp(opname, "Ending"))
    opval= options->ending;
  else if (!stricmp(opname, "Compressed"))
    opval= (options->compressed);

  if (opval == (ulonglong)NO_IVAL && options && options->oplist)
    if ((pv= GetListOption(xp->g, opname, options->oplist)))
      opval= CharToNumber(pv, strlen(pv), ULONGLONG_MAX, true);

  return (int)opval;
} // end of GetIntegerOption

/****************************************************************************/
/*  Set the value of the opname option (does not work for oplist options)   */
/*  Currently used only to set the Lrecl value.                             */
/****************************************************************************/
bool ha_connect::SetIntegerOption(char *opname, int n)
{
  PTOS options= GetTableOptionStruct();

  if (!options)
    return true;

  if (!stricmp(opname, "Lrecl"))
    options->lrecl= n;
  else if (!stricmp(opname, "Elements"))
    options->elements= n;
//else if (!stricmp(opname, "Estimate"))
//  options->estimate= n;
  else if (!stricmp(opname, "Multiple"))
    options->multiple= n;
  else if (!stricmp(opname, "Header"))
    options->header= n;
  else if (!stricmp(opname, "Quoted"))
    options->quoted= n;
  else if (!stricmp(opname, "Ending"))
    options->ending= n;
  else if (!stricmp(opname, "Compressed"))
    options->compressed= n;
  else
    return true;
//else if (options->oplist)
//  SetListOption(opname, options->oplist, n);

  return false;
} // end of SetIntegerOption

/****************************************************************************/
/*  Return a field option structure.                                        */
/****************************************************************************/
PFOS ha_connect::GetFieldOptionStruct(Field *fdp)
{
  return fdp->option_struct;
} // end of GetFildOptionStruct

/****************************************************************************/
/*  Returns the column description structure used to make the column.       */
/****************************************************************************/
void *ha_connect::GetColumnOption(PGLOBAL g, void *field, PCOLINFO pcf)
{
  const char *cp;
  char   *chset, v;
  ha_field_option_struct *fop;
  Field*  fp;
  Field* *fldp;

  // Double test to be on the safe side
  if (!table)
    return NULL;

  // Find the column to describe
  if (field) {
    fldp= (Field**)field;
    fldp++;
  } else
    fldp= (tshp) ? tshp->field : table->field;

  if (!fldp || !(fp= *fldp))
    return NULL;

  // Get the CONNECT field options structure
  fop= GetFieldOptionStruct(fp);
  pcf->Flags= 0;

  // Now get column information
  pcf->Name= (char*)fp->field_name;

  if (fop && fop->special) {
    pcf->Fieldfmt= (char*)fop->special;
    pcf->Flags= U_SPECIAL;
    return fldp;
    } // endif special

  pcf->Scale= 0;
  pcf->Opt= (fop) ? (int)fop->opt : 0;

  if ((pcf->Length= fp->field_length) < 0)
    pcf->Length= 256;            // BLOB?

  pcf->Precision= pcf->Length;

  if (fop) {
    pcf->Offset= (int)fop->offset;
    pcf->Freq= (int)fop->freq;
    pcf->Datefmt= (char*)fop->dateformat;
    pcf->Fieldfmt= (char*)fop->fieldformat;
  } else {
    pcf->Offset= -1;
    pcf->Freq= 0;
    pcf->Datefmt= NULL;
    pcf->Fieldfmt= NULL;
  } // endif fop

  chset = (char *)fp->charset()->name;
  v = (!strcmp(chset, "binary")) ? 'B' : 0;

  switch (fp->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
      pcf->Flags |= U_VAR;
      /* no break */
    default:
      pcf->Type= MYSQLtoPLG(fp->type(), &v);
      break;
    } // endswitch SQL type

  switch (pcf->Type) {
    case TYPE_STRING:
      // Do something for case
      cp= fp->charset()->name;

      // Find if collation name ends by _ci
      if (!strcmp(cp + strlen(cp) - 3, "_ci")) {
        pcf->Scale= 1;     // Case insensitive
        pcf->Opt= 0;       // Prevent index opt until it is safe
        } // endif ci

      break;
    case TYPE_DOUBLE:
      pcf->Scale= MY_MAX(MY_MIN(fp->decimals(), ((unsigned)pcf->Length - 2)), 0);
      break;
    case TYPE_DECIM:
      pcf->Precision= ((Field_new_decimal*)fp)->precision;
      pcf->Length= pcf->Precision;
      pcf->Scale= fp->decimals();
      break;
    case TYPE_DATE:
      // Field_length is only used for DATE columns
      if (fop && fop->fldlen)
        pcf->Length= (int)fop->fldlen;
      else {
        int len;

        if (pcf->Datefmt) {
          // Find the (max) length produced by the date format
          char    buf[256];
          PGLOBAL g= GetPlug(table->in_use, xp);
          PDTP    pdtp= MakeDateFormat(g, pcf->Datefmt, false, true, 0);
          struct tm datm;
          bzero(&datm, sizeof(datm));
          datm.tm_mday= 12;
          datm.tm_mon= 11;
          datm.tm_year= 112;
          len= strftime(buf, 256, pdtp->OutFmt, &datm);
        } else
          len= 0;

        // 11 is for signed numeric representation of the date
        pcf->Length= (len) ? len : 11;
        } // endelse

      break;
    default:
      break;
    } // endswitch type

  if (fp->flags & UNSIGNED_FLAG)
    pcf->Flags |= U_UNSIGNED;

  if (fp->flags & ZEROFILL_FLAG)
    pcf->Flags |= U_ZEROFILL;

  // This is used to skip null bit
  if (fp->real_maybe_null())
    pcf->Flags |= U_NULLS;

  // Mark virtual columns as such
  if (fp->vcol_info && !fp->stored_in_db)
    pcf->Flags |= U_VIRTUAL;

  pcf->Key= 0;   // Not used when called from MySQL

  // Get the comment if any
  if (fp->comment.str && fp->comment.length) {
    pcf->Remark= (char*)PlugSubAlloc(g, NULL, fp->comment.length + 1);
    memcpy(pcf->Remark, fp->comment.str, fp->comment.length);
    pcf->Remark[fp->comment.length]= 0;
  } else
    pcf->Remark= NULL;

  return fldp;
} // end of GetColumnOption

/****************************************************************************/
/*  Return an index option structure.                                       */
/****************************************************************************/
PXOS ha_connect::GetIndexOptionStruct(KEY *kp)
{
  return kp->option_struct;
} // end of GetIndexOptionStruct

/****************************************************************************/
/*  Return a Boolean index option or false if not specified.                */
/****************************************************************************/
bool ha_connect::GetIndexOption(KEY *kp, char *opname)
{
  bool opval= false;
  PXOS options= GetIndexOptionStruct(kp);

  if (options) {
    if (!stricmp(opname, "Dynamic"))
      opval= options->dynamic;
    else if (!stricmp(opname, "Mapped"))
      opval= options->mapped;

  } else if (kp->comment.str != NULL) {
    char *pv, *oplist= kp->comment.str;

    if ((pv= GetListOption(xp->g, opname, oplist)))
      opval= (!*pv || *pv == 'y' || *pv == 'Y' || atoi(pv) != 0);

  } // endif comment

  return opval;
} // end of GetIndexOption

/****************************************************************************/
/*  Returns the index description structure used to make the index.         */
/****************************************************************************/
bool ha_connect::IsUnique(uint n)
{
  TABLE_SHARE *s= (table) ? table->s : NULL;
  KEY          kp= s->key_info[n];

  return (kp.flags & 1) != 0;
} // end of IsUnique

/****************************************************************************/
/*  Returns the index description structure used to make the index.         */
/****************************************************************************/
PIXDEF ha_connect::GetIndexInfo(TABLE_SHARE *s)
{
  char    *name, *pn;
  bool     unique;
  PIXDEF   xdp, pxd=NULL, toidx= NULL;
  PKPDEF   kpp, pkp;
  KEY      kp;
  PGLOBAL& g= xp->g;

  if (!s)
    s= table->s;

  for (int n= 0; (unsigned)n < s->keynames.count; n++) {
    if (xtrace)
      htrc("Getting created index %d info\n", n + 1);

    // Find the index to describe
    kp= s->key_info[n];

    // Now get index information
    pn= (char*)s->keynames.type_names[n];
    name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
    strcpy(name, pn);    // This is probably unuseful
    unique= (kp.flags & 1) != 0;
    pkp= NULL;

    // Allocate the index description block
    xdp= new(g) INDEXDEF(name, unique, n);

    // Get the the key parts info
    for (int k= 0; (unsigned)k < kp.user_defined_key_parts; k++) {
      pn= (char*)kp.key_part[k].field->field_name;
      name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
      strcpy(name, pn);    // This is probably unuseful

      // Allocate the key part description block
      kpp= new(g) KPARTDEF(name, k + 1);
      kpp->SetKlen(kp.key_part[k].length);

#if 0             // NIY
    // Index on auto increment column can be an XXROW index
    if (kp.key_part[k].field->flags & AUTO_INCREMENT_FLAG &&
        kp.uder_defined_key_parts == 1) {
      char   *type= GetStringOption("Type", "DOS");
      TABTYPE typ= GetTypeID(type);

      xdp->SetAuto(IsTypeFixed(typ));
      } // endif AUTO_INCREMENT
#endif // 0

      if (pkp)
        pkp->SetNext(kpp);
      else
        xdp->SetToKeyParts(kpp);

      pkp= kpp;
      } // endfor k

    xdp->SetNParts(kp.user_defined_key_parts);
    xdp->Dynamic= GetIndexOption(&kp, "Dynamic");
    xdp->Mapped= GetIndexOption(&kp, "Mapped");

    if (pxd)
      pxd->SetNext(xdp);
    else
      toidx= xdp;

    pxd= xdp;
    } // endfor n

  return toidx;
} // end of GetIndexInfo

bool ha_connect::IsPartitioned(void)
{
  if (tshp)
    return tshp->partition_info_str_len > 0;
  else if (table && table->part_info)
    return true;
  else
    return false;

} // end of IsPartitioned

const char *ha_connect::GetDBName(const char* name)
{
  return (name) ? name : table->s->db.str;
} // end of GetDBName

const char *ha_connect::GetTableName(void)
{
  return (tshp) ? tshp->table_name.str : table_share->table_name.str;
} // end of GetTableName

char *ha_connect::GetPartName(void)
{
  return (IsPartitioned()) ? partname : (char*)GetTableName();
} // end of GetTableName

#if 0
/****************************************************************************/
/*  Returns the column real or special name length of a field.              */
/****************************************************************************/
int ha_connect::GetColNameLen(Field *fp)
{
  int n;
  PFOS fop= GetFieldOptionStruct(fp);

  // Now get the column name length
  if (fop && fop->special)
    n= strlen(fop->special) + 1;
  else
    n= strlen(fp->field_name);

  return n;
} // end of GetColNameLen

/****************************************************************************/
/*  Returns the column real or special name of a field.                     */
/****************************************************************************/
char *ha_connect::GetColName(Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  return (fop && fop->special) ? fop->special : (char*)fp->field_name;
} // end of GetColName

/****************************************************************************/
/*  Adds the column real or special name of a field to a string.            */
/****************************************************************************/
void ha_connect::AddColName(char *cp, Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  // Now add the column name
  if (fop && fop->special)
    // The prefix * mark the column as "special"
    strcat(strcpy(cp, "*"), strupr(fop->special));
  else
    strcpy(cp, (char*)fp->field_name);

} // end of AddColName
#endif // 0

/***********************************************************************/
/*  This function sets the current database path.                      */
/***********************************************************************/
void ha_connect::SetDataPath(PGLOBAL g, const char *path) 
{
  datapath= SetPath(g, path);
} // end of SetDataPath

/****************************************************************************/
/*  Get the table description block of a CONNECT table.                     */
/****************************************************************************/
PTDB ha_connect::GetTDB(PGLOBAL g)
{
  const char *table_name;
  PTDB        tp;

  // Double test to be on the safe side
  if (!g || !table)
    return NULL;

  table_name= GetTableName();

  if (!xp->CheckQuery(valid_query_id) && tdbp
                      && !stricmp(tdbp->GetName(), table_name)
                      && (tdbp->GetMode() == xmod
                       || (tdbp->GetMode() == MODE_READ && xmod == MODE_READX)
                       || tdbp->GetAmType() == TYPE_AM_XML)) {
    tp= tdbp;
    tp->SetMode(xmod);
  } else if ((tp= CntGetTDB(g, table_name, xmod, this))) {
    valid_query_id= xp->last_query_id;
//  tp->SetMode(xmod);
  } else
    htrc("GetTDB: %s\n", g->Message);

  return tp;
} // end of GetTDB

/****************************************************************************/
/*  Open a CONNECT table, restricting column list if cols is true.          */
/****************************************************************************/
int ha_connect::OpenTable(PGLOBAL g, bool del)
{
  bool  rc= false;
  char *c1= NULL, *c2=NULL;

  // Double test to be on the safe side
  if (!g || !table) {
    htrc("OpenTable logical error; g=%p table=%p\n", g, table);
    return HA_ERR_INITIALIZATION;
    } // endif g

  if (!(tdbp= GetTDB(g)))
    return RC_FX;
  else if (tdbp->IsReadOnly())
    switch (xmod) {
      case MODE_WRITE:
      case MODE_INSERT:
      case MODE_UPDATE:
      case MODE_DELETE:
        strcpy(g->Message, MSG(READ_ONLY));
        return HA_ERR_TABLE_READONLY;
      default:
        break;
      } // endswitch xmode

  if (xmod != MODE_INSERT || tdbp->GetAmType() == TYPE_AM_ODBC
                          || tdbp->GetAmType() == TYPE_AM_MYSQL) {
    // Get the list of used fields (columns)
    char        *p;
    unsigned int k1, k2, n1, n2;
    Field*      *field;
    Field*       fp;
    MY_BITMAP   *map= (xmod == MODE_INSERT) ? table->write_set : table->read_set;
    MY_BITMAP   *ump= (xmod == MODE_UPDATE) ? table->write_set : NULL;

    k1= k2= 0;
    n1= n2= 1;         // 1 is space for final null character

    for (field= table->field; fp= *field; field++) {
      if (bitmap_is_set(map, fp->field_index)) {
        n1+= (strlen(fp->field_name) + 1);
        k1++;
        } // endif

      if (ump && bitmap_is_set(ump, fp->field_index)) {
        n2+= (strlen(fp->field_name) + 1);
        k2++;
        } // endif

      } // endfor field

    if (k1) {
      p= c1= (char*)PlugSubAlloc(g, NULL, n1);

      for (field= table->field; fp= *field; field++)
        if (bitmap_is_set(map, fp->field_index)) {
          strcpy(p, (char*)fp->field_name);
          p+= (strlen(p) + 1);
          } // endif used field

      *p= '\0';          // mark end of list
      } // endif k1

    if (k2) {
      p= c2= (char*)PlugSubAlloc(g, NULL, n2);

      for (field= table->field; fp= *field; field++)
        if (bitmap_is_set(ump, fp->field_index)) {
          strcpy(p, (char*)fp->field_name);

          if (part_id && bitmap_is_set(part_id, fp->field_index)) {
            // Trying to update a column used for partitioning
            // This cannot be currently done because it may require
            // a row to be moved in another partition.
            sprintf(g->Message, 
              "Cannot update column %s because it is used for partitioning",
              p);
            return HA_ERR_INTERNAL_ERROR;
            } // endif part_id

          p+= (strlen(p) + 1);
          } // endif used field

      *p= '\0';          // mark end of list
      } // endif k2

    } // endif xmod

  // Open the table
  if (!(rc= CntOpenTable(g, tdbp, xmod, c1, c2, del, this))) {
    istable= true;
//  strmake(tname, table_name, sizeof(tname)-1);

    // We may be in a create index query
    if (xmod == MODE_ANY && *tdbp->GetName() != '#') {
      // The current indexes
      PIXDEF oldpix= GetIndexInfo();
      } // endif xmod

  } else
    htrc("OpenTable: %s\n", g->Message);

  if (rc) {
    tdbp= NULL;
    valid_info= false;
    } // endif rc

  return (rc) ? HA_ERR_INITIALIZATION : 0;
} // end of OpenTable


/****************************************************************************/
/*  CheckColumnList: check that all bitmap columns do exist.                */
/****************************************************************************/
bool ha_connect::CheckColumnList(PGLOBAL g)
{
  // Check the list of used fields (columns)
  int        rc;
  bool       brc= false;
  PCOL       colp;
  Field*    *field;
  Field*     fp;
  MY_BITMAP *map= table->read_set;

  // Save stack and allocation environment and prepare error return
  if (g->jump_level == MAX_JUMP) {
    strcpy(g->Message, MSG(TOO_MANY_JUMPS));
    return true;
    } // endif jump_level

  if ((rc= setjmp(g->jumper[++g->jump_level])) == 0) {
    for (field= table->field; fp= *field; field++)
      if (bitmap_is_set(map, fp->field_index)) {
        if (!(colp= tdbp->ColDB(g, (PSZ)fp->field_name, 0))) {
          sprintf(g->Message, "Column %s not found in %s", 
                  fp->field_name, tdbp->GetName());
          brc= true;
          goto fin;
          } // endif colp

        if ((brc= colp->InitValue(g)))
          goto fin;

        colp->AddColUse(U_P);           // For PLG tables
        } // endif

  } else
    brc= true;

 fin:
  g->jump_level--;
  return brc;
} // end of CheckColumnList


/****************************************************************************/
/*  IsOpened: returns true if the table is already opened.                  */
/****************************************************************************/
bool ha_connect::IsOpened(void)
{
  return (!xp->CheckQuery(valid_query_id) && tdbp
                                          && tdbp->GetUse() == USE_OPEN);
} // end of IsOpened


/****************************************************************************/
/*  Close a CONNECT table.                                                  */
/****************************************************************************/
int ha_connect::CloseTable(PGLOBAL g)
{
  int rc= CntCloseTable(g, tdbp, nox, abort);
  tdbp= NULL;
  sdvalin=NULL;
  sdvalout=NULL;
  valid_info= false;
  indexing= -1;
  nox= true;
  abort= false;
  return rc;
} // end of CloseTable


/***********************************************************************/
/*  Make a pseudo record from current row values. Specific to MySQL.   */
/***********************************************************************/
int ha_connect::MakeRecord(char *buf)
{
  char          *p, *fmt, val[32];
  int            rc= 0;
  Field*        *field;
  Field         *fp;
  my_bitmap_map *org_bitmap;
  CHARSET_INFO  *charset= tdbp->data_charset();
//MY_BITMAP      readmap;
  MY_BITMAP     *map;
  PVAL           value;
  PCOL           colp= NULL;
  DBUG_ENTER("ha_connect::MakeRecord");

  if (xtrace > 1)
    htrc("Maps: read=%08X write=%08X vcol=%08X defr=%08X defw=%08X\n",
            *table->read_set->bitmap, *table->write_set->bitmap,
            *table->vcol_set->bitmap,
            *table->def_read_set.bitmap, *table->def_write_set.bitmap);

  // Avoid asserts in field::store() for columns that are not updated
  org_bitmap= dbug_tmp_use_all_columns(table, table->write_set);

  // This is for variable_length rows
  memset(buf, 0, table->s->null_bytes);

  // When sorting read_set selects all columns, so we use def_read_set
  map= (MY_BITMAP *)&table->def_read_set;

  // Make the pseudo record from field values
  for (field= table->field; *field && !rc; field++) {
    fp= *field;

    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column

    if (bitmap_is_set(map, fp->field_index) || alter) {
      // This is a used field, fill the buffer with value
      for (colp= tdbp->GetColumns(); colp; colp= colp->GetNext())
        if ((!mrr || colp->GetKcol()) &&
            !stricmp(colp->GetName(), (char*)fp->field_name))
          break;

      if (!colp) {
        if (mrr)
          continue;

        htrc("Column %s not found\n", fp->field_name);
        dbug_tmp_restore_column_map(table->write_set, org_bitmap);
        DBUG_RETURN(HA_ERR_WRONG_IN_RECORD);
        } // endif colp

      value= colp->GetValue();
      p= NULL;

      // All this was better optimized
      if (!value->IsNull()) {
        switch (value->GetType()) {
          case TYPE_DATE:
            if (!sdvalout)
              sdvalout= AllocateValue(xp->g, TYPE_STRING, 20);

            switch (fp->type()) {
              case MYSQL_TYPE_DATE:
                fmt= "%Y-%m-%d";
                break;
              case MYSQL_TYPE_TIME:
                fmt= "%H:%M:%S";
                break;
              case MYSQL_TYPE_YEAR:
                fmt= "%Y";
                break;
              default:
                fmt= "%Y-%m-%d %H:%M:%S";
                break;
              } // endswitch type

            // Get date in the format required by MySQL fields
            value->FormatValue(sdvalout, fmt);
            p= sdvalout->GetCharValue();
            rc= fp->store(p, strlen(p), charset, CHECK_FIELD_WARN);
            break;
          case TYPE_STRING:
          case TYPE_DECIM:
            p= value->GetCharString(val);
            charset= tdbp->data_charset();
            rc= fp->store(p, strlen(p), charset, CHECK_FIELD_WARN);
            break;
          case TYPE_DOUBLE:
            rc= fp->store(value->GetFloatValue());
            break;
          default:
            rc= fp->store(value->GetBigintValue(), value->IsUnsigned());
            break;
          } // endswitch Type

        // Store functions returns 1 on overflow and -1 on fatal error
        if (rc > 0) {
          char buf[256];
          THD *thd= ha_thd();

          sprintf(buf, "Out of range value %.140s for column '%s' at row %ld",
            value->GetCharString(val),
            fp->field_name, 
            thd->get_stmt_da()->current_row_for_warning());

          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, buf);
          DBUG_PRINT("MakeRecord", ("%s", buf));
          rc= 0;
        } else if (rc < 0)
          rc= HA_ERR_WRONG_IN_RECORD;

        fp->set_notnull();
      } else
        fp->set_null();

      } // endif bitmap

    } // endfor field

  // This is sometimes required for partition tables because the buf
  // can be different from the table->record[0] buffer
  if (buf != (char*)table->record[0])
    memcpy(buf, table->record[0], table->s->stored_rec_length);

  // This is copied from ha_tina and is necessary to avoid asserts
  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
  DBUG_RETURN(rc);
} // end of MakeRecord


/***********************************************************************/
/*  Set row values from a MySQL pseudo record. Specific to MySQL.      */
/***********************************************************************/
int ha_connect::ScanRecord(PGLOBAL g, uchar *buf)
{
  char    attr_buffer[1024];
  char    data_buffer[1024];
  char   *fmt;
  int     rc= 0;
  PCOL    colp;
  PVAL    value;
  Field  *fp;
  PTDBASE tp= (PTDBASE)tdbp;
  String  attribute(attr_buffer, sizeof(attr_buffer),
                    table->s->table_charset);
  my_bitmap_map *bmap= dbug_tmp_use_all_columns(table, table->read_set);
  const CHARSET_INFO *charset= tdbp->data_charset();
  String  data_charset_value(data_buffer, sizeof(data_buffer),  charset);

  // Scan the pseudo record for field values and set column values
  for (Field **field=table->field ; *field ; field++) {
    fp= *field;

    if ((fp->vcol_info && !fp->stored_in_db) ||
         fp->option_struct->special)
      continue;            // Is a virtual column possible here ???

    if ((xmod == MODE_INSERT && tdbp->GetAmType() != TYPE_AM_MYSQL
                             && tdbp->GetAmType() != TYPE_AM_ODBC) ||
        bitmap_is_set(table->write_set, fp->field_index)) {
      for (colp= tp->GetSetCols(); colp; colp= colp->GetNext())
        if (!stricmp(colp->GetName(), fp->field_name))
          break;

      if (!colp) {
        htrc("Column %s not found\n", fp->field_name);
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
      } else
        value= colp->GetValue();

      // This is a used field, fill the value from the row buffer
      // All this could be better optimized
      if (fp->is_null()) {
        if (colp->IsNullable())
          value->SetNull(true);

        value->Reset();
      } else switch (value->GetType()) {
        case TYPE_DOUBLE:
          value->SetValue(fp->val_real());
          break;
        case TYPE_DATE:
          if (!sdvalin)
            sdvalin= (DTVAL*)AllocateValue(xp->g, TYPE_DATE, 19);

          // Get date in the format produced by MySQL fields
          switch (fp->type()) {
            case MYSQL_TYPE_DATE:
              fmt= "YYYY-MM-DD";
              break;
            case MYSQL_TYPE_TIME:
              fmt= "hh:mm:ss";
              break;
            case MYSQL_TYPE_YEAR:
              fmt= "YYYY";
              break;
            default:
              fmt= "YYYY-MM-DD hh:mm:ss";
            } // endswitch type

          ((DTVAL*)sdvalin)->SetFormat(g, fmt, strlen(fmt));
          fp->val_str(&attribute);
          sdvalin->SetValue_psz(attribute.c_ptr_safe());
          value->SetValue_pval(sdvalin);
          break;
        default:
          fp->val_str(&attribute);

          if (charset != &my_charset_bin) {
            // Convert from SQL field charset to DATA_CHARSET
            uint cnv_errors;

            data_charset_value.copy(attribute.ptr(), attribute.length(),
                                    attribute.charset(), charset, &cnv_errors);
            value->SetValue_psz(data_charset_value.c_ptr_safe());
          } else
            value->SetValue_psz(attribute.c_ptr_safe());

          break;
        } // endswitch Type

#ifdef NEWCHANGE
    } else if (xmod == MODE_UPDATE) {
      PCOL cp;

      for (cp= tp->GetColumns(); cp; cp= cp->GetNext())
        if (!stricmp(colp->GetName(), cp->GetName()))
          break;

      if (!cp) {
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
        } // endif cp

      value->SetValue_pval(cp->GetValue());
    } else // mode Insert
      value->Reset();
#else
    } // endif bitmap_is_set
#endif

    } // endfor field

 err:
  dbug_tmp_restore_column_map(table->read_set, bmap);
  return rc;
} // end of ScanRecord


/***********************************************************************/
/*  Check change in index column. Specific to MySQL.                   */
/*  Should be elaborated to check for real changes.                    */
/***********************************************************************/
int ha_connect::CheckRecord(PGLOBAL g, const uchar *oldbuf, uchar *newbuf)
{
  return ScanRecord(g, newbuf);
} // end of dummy CheckRecord


/***********************************************************************/
/*  Return the where clause for remote indexed read.                   */
/***********************************************************************/
bool ha_connect::MakeKeyWhere(PGLOBAL g, char *qry, OPVAL op, char *q, 
                                         const void *key, int klen)
{
  const uchar   *ptr;
  uint           rem, len, stlen; //, prtlen;
  bool           nq, b= false;
  Field         *fp;
  KEY           *kfp;
  KEY_PART_INFO *kpart;

  if (active_index == MAX_KEY)
    return false;
  else if (!key) {
    strcpy(g->Message, "MakeKeyWhere: No key");
    return true;
  } // endif key

  strcat(qry, " WHERE (");
  kfp= &table->key_info[active_index];
  rem= kfp->user_defined_key_parts,
  len= klen,
  ptr= (const uchar *)key;

  for (kpart= kfp->key_part; rem; rem--, kpart++) {
    fp= kpart->field;
    stlen= kpart->store_length;
//  prtlen= MY_MIN(stlen, len);
    nq= fp->str_needs_quotes();

    if (b)
      strcat(qry, " AND ");
    else
      b= true;

    strcat(strncat(strcat(qry, q), fp->field_name, strlen(fp->field_name)), q);

    switch (op) {
      case OP_EQ:
      case OP_GT:
      case OP_GE:
        strcat(qry, GetValStr(op, false));
        break;
      default:
        strcat(qry, " ??? ");
      } // endwitch op

    if (nq)
      strcat(qry, "'");

    if (kpart->key_part_flag & HA_VAR_LENGTH_PART) {
      String varchar;
      uint   var_length= uint2korr(ptr);

      varchar.set_quick((char*) ptr+HA_KEY_BLOB_LENGTH,
                      var_length, &my_charset_bin);
      strncat(qry, varchar.ptr(), varchar.length());
    } else {
      char   strbuff[MAX_FIELD_WIDTH];
      String str(strbuff, sizeof(strbuff), kpart->field->charset()), *res;

      res= fp->val_str(&str, ptr);
      strncat(qry, res->ptr(), res->length());
    } // endif flag

    if (nq)
      strcat(qry, "'");

    if (stlen >= len)
      break;

    len-= stlen;

    /* For nullable columns, null-byte is already skipped before, that is
      ptr was incremented by 1. Since store_length still counts null-byte,
      we need to subtract 1 from store_length. */
    ptr+= stlen - MY_TEST(kpart->null_bit);
    } // endfor kpart

  strcat(qry, ")");
  return false;
} // end of MakeKeyWhere


/***********************************************************************/
/*  Return the string representing an operator.                        */
/***********************************************************************/
const char *ha_connect::GetValStr(OPVAL vop, bool neg)
{
  const char *val;

  switch (vop) {
    case OP_EQ:
      val= " = ";
      break;
    case OP_NE:
      val= " <> ";
      break;
    case OP_GT:
      val= " > ";
      break;
    case OP_GE:
      val= " >= ";
      break;
    case OP_LT:
      val= " < ";
      break;
    case OP_LE:
      val= " <= ";
      break;
    case OP_IN:
      val= (neg) ? " NOT IN (" : " IN (";
      break;
    case OP_NULL:
      val= (neg) ? " IS NOT NULL" : " IS NULL";
      break;
    case OP_LIKE:
      val= " LIKE ";
      break;
    case OP_XX:
      val= (neg) ? " NOT BETWEEN " : " BETWEEN ";
      break;
    case OP_EXIST:
      val= (neg) ? " NOT EXISTS " : " EXISTS ";
      break;
    case OP_AND:
      val= " AND ";
      break;
    case OP_OR:
      val= " OR ";
      break;
    case OP_NOT:
      val= " NOT ";
      break;
    case OP_CNC:
      val= " || ";
      break;
    case OP_ADD:
      val= " + ";
      break;
    case OP_SUB:
      val= " - ";
      break;
    case OP_MULT:
      val= " * ";
      break;
    case OP_DIV:
      val= " / ";
      break;
    default:
      val= " ? ";
      break;
    } /* endswitch */

  return val;
} // end of GetValStr

#if 0
/***********************************************************************/
/*  Check the WHERE condition and return a CONNECT filter.             */
/***********************************************************************/
PFIL ha_connect::CheckFilter(PGLOBAL g)
{
  return CondFilter(g, (Item *)pushed_cond);
} // end of CheckFilter
#endif // 0

/***********************************************************************/
/*  Check the WHERE condition and return a CONNECT filter.             */
/***********************************************************************/
PFIL ha_connect::CondFilter(PGLOBAL g, Item *cond)
{
  unsigned int i;
  bool  ismul= false;
  OPVAL vop= OP_XX;
  PFIL  filp= NULL;

  if (!cond)
    return NULL;

  if (xtrace)
    htrc("Cond type=%d\n", cond->type());

  if (cond->type() == COND::COND_ITEM) {
    PFIL       fp;
    Item_cond *cond_item= (Item_cond *)cond;

    if (xtrace)
      htrc("Cond: Ftype=%d name=%s\n", cond_item->functype(),
                                       cond_item->func_name());

    switch (cond_item->functype()) {
      case Item_func::COND_AND_FUNC: vop= OP_AND; break;
      case Item_func::COND_OR_FUNC:  vop= OP_OR;  break;
      default: return NULL;
      } // endswitch functype

    List<Item>* arglist= cond_item->argument_list();
    List_iterator<Item> li(*arglist);
    Item *subitem;

    for (i= 0; i < arglist->elements; i++)
      if ((subitem= li++)) {
        if (!(fp= CondFilter(g, subitem))) {
          if (vop == OP_OR)
            return NULL;
        } else
          filp= (filp) ? MakeFilter(g, filp, vop, fp) : fp;

      } else
        return NULL;

  } else if (cond->type() == COND::FUNC_ITEM) {
    unsigned int i;
    bool       iscol, neg= FALSE;
    PCOL       colp[2]= {NULL,NULL};
    PPARM      pfirst= NULL, pprec= NULL;
    POPER      pop;
    Item_func *condf= (Item_func *)cond;
    Item*     *args= condf->arguments();

    if (xtrace)
      htrc("Func type=%d argnum=%d\n", condf->functype(),
                                         condf->argument_count());

    switch (condf->functype()) {
      case Item_func::EQUAL_FUNC:
      case Item_func::EQ_FUNC: vop= OP_EQ;  break;
      case Item_func::NE_FUNC: vop= OP_NE;  break;
      case Item_func::LT_FUNC: vop= OP_LT;  break;
      case Item_func::LE_FUNC: vop= OP_LE;  break;
      case Item_func::GE_FUNC: vop= OP_GE;  break;
      case Item_func::GT_FUNC: vop= OP_GT;  break;
      case Item_func::IN_FUNC: vop= OP_IN;
      case Item_func::BETWEEN:
        ismul= true;
        neg= ((Item_func_opt_neg *)condf)->negated;
        break;
      default: return NULL;
      } // endswitch functype

    pop= (POPER)PlugSubAlloc(g, NULL, sizeof(OPER));
    pop->Name= NULL;
    pop->Val=vop;
    pop->Mod= 0;

    if (condf->argument_count() < 2)
      return NULL;

    for (i= 0; i < condf->argument_count(); i++) {
      if (xtrace)
        htrc("Argtype(%d)=%d\n", i, args[i]->type());

      if (i >= 2 && !ismul) {
        if (xtrace)
          htrc("Unexpected arg for vop=%d\n", vop);

        continue;
        } // endif i

      if ((iscol= args[i]->type() == COND::FIELD_ITEM)) {
        Item_field *pField= (Item_field *)args[i];

        // IN and BETWEEN clauses should be col VOP list
        if (i && ismul)
          return NULL;

        if (pField->field->table != table ||
            !(colp[i]= tdbp->ColDB(g, (PSZ)pField->field->field_name, 0)))
          return NULL;  // Column does not belong to this table

        if (xtrace) {
          htrc("Field index=%d\n", pField->field->field_index);
          htrc("Field name=%s\n", pField->field->field_name);
          } // endif xtrace

      } else {
        char    buff[256];
        String *res, tmp(buff, sizeof(buff), &my_charset_bin);
        Item_basic_constant *pval= (Item_basic_constant *)args[i];
        PPARM pp= (PPARM)PlugSubAlloc(g, NULL, sizeof(PARM));

        // IN and BETWEEN clauses should be col VOP list
        if (!i && (ismul))
          return NULL;

        if ((res= pval->val_str(&tmp)) == NULL)
          return NULL;                      // To be clarified

        switch (args[i]->real_type()) {
          case COND::STRING_ITEM:
            pp->Type= TYPE_STRING;
            pp->Value= PlugSubAlloc(g, NULL, res->length() + 1);
            strncpy((char*)pp->Value, res->ptr(), res->length() + 1);
            break;
          case COND::INT_ITEM:
            pp->Type= TYPE_INT;
            pp->Value= PlugSubAlloc(g, NULL, sizeof(int));
            *((int*)pp->Value)= (int)pval->val_int();
            break;
          case COND::DATE_ITEM:
            pp->Type= TYPE_DATE;
            pp->Value= PlugSubAlloc(g, NULL, sizeof(int));
            *((int*)pp->Value)= (int)pval->val_int_from_date();
            break;
          case COND::REAL_ITEM:
            pp->Type= TYPE_DOUBLE;
            pp->Value= PlugSubAlloc(g, NULL, sizeof(double));
            *((double*)pp->Value)= pval->val_real();
            break;
          case COND::DECIMAL_ITEM:
            pp->Type= TYPE_DOUBLE;
            pp->Value= PlugSubAlloc(g, NULL, sizeof(double));
            *((double*)pp->Value)= pval->val_real_from_decimal();
            break;
          case COND::CACHE_ITEM:    // Possible ???
          case COND::NULL_ITEM:     // TODO: handle this
          default:
            return NULL;
          } // endswitch type

        if (xtrace)
          htrc("Value=%.*s\n", res->length(), res->ptr());

        // Append the value to the argument list
        if (pprec)
          pprec->Next= pp;
        else
          pfirst= pp;

        pp->Domain= i;
        pp->Next= NULL;
        pprec= pp;
      } // endif type

      } // endfor i

    filp= MakeFilter(g, colp, pop, pfirst, neg);
  } else {
    if (xtrace)
      htrc("Unsupported condition\n");

    return NULL;
  } // endif's type

  return filp;
} // end of CondFilter

/***********************************************************************/
/*  Check the WHERE condition and return a MYSQL/ODBC/WQL filter.      */
/***********************************************************************/
PCFIL ha_connect::CheckCond(PGLOBAL g, PCFIL filp, AMT tty, Item *cond)
{
  char *body= filp->Body;
  unsigned int i;
  bool  ismul= false, x= (tty == TYPE_AM_MYX || tty == TYPE_AM_XDBC);
  OPVAL vop= OP_XX;

  if (!cond)
    return NULL;

  if (xtrace)
    htrc("Cond type=%d\n", cond->type());

  if (cond->type() == COND::COND_ITEM) {
    char      *p1, *p2;
    Item_cond *cond_item= (Item_cond *)cond;

    if (x)
      return NULL;

    if (xtrace)
      htrc("Cond: Ftype=%d name=%s\n", cond_item->functype(),
                                         cond_item->func_name());

    switch (cond_item->functype()) {
      case Item_func::COND_AND_FUNC: vop= OP_AND; break;
      case Item_func::COND_OR_FUNC:  vop= OP_OR;  break;
      default: return NULL;
      } // endswitch functype

    List<Item>* arglist= cond_item->argument_list();
    List_iterator<Item> li(*arglist);
    Item *subitem;

    p1= body + strlen(body);
    strcpy(p1, "(");
    p2= p1 + 1;

    for (i= 0; i < arglist->elements; i++)
      if ((subitem= li++)) {
        if (!CheckCond(g, filp, tty, subitem)) {
          if (vop == OP_OR)
            return NULL;
          else
            *p2= 0;

        } else {
          p1= p2 + strlen(p2);
          strcpy(p1, GetValStr(vop, false));
          p2= p1 + strlen(p1);
        } // endif CheckCond

      } else
        return NULL;

    if (*p1 != '(')
      strcpy(p1, ")");
    else
      return NULL;

  } else if (cond->type() == COND::FUNC_ITEM) {
    unsigned int i;
//  int   n;
    bool       iscol, neg= FALSE;
    Item_func *condf= (Item_func *)cond;
    Item*     *args= condf->arguments();

    if (xtrace)
      htrc("Func type=%d argnum=%d\n", condf->functype(),
                                         condf->argument_count());

//  neg= condf->

    switch (condf->functype()) {
      case Item_func::EQUAL_FUNC:
      case Item_func::EQ_FUNC: vop= OP_EQ;  break;
      case Item_func::NE_FUNC: vop= OP_NE;  break;
      case Item_func::LT_FUNC: vop= OP_LT;  break;
      case Item_func::LE_FUNC: vop= OP_LE;  break;
      case Item_func::GE_FUNC: vop= OP_GE;  break;
      case Item_func::GT_FUNC: vop= OP_GT;  break;
      case Item_func::IN_FUNC: vop= OP_IN;
      case Item_func::BETWEEN:
        ismul= true;
        neg= ((Item_func_opt_neg *)condf)->negated;
        break;
      default: return NULL;
      } // endswitch functype

    if (condf->argument_count() < 2)
      return NULL;
    else if (ismul && tty == TYPE_AM_WMI)
      return NULL;        // Not supported by WQL

    if (x && (neg || !(vop == OP_EQ || vop == OP_IN)))
      return NULL;

    for (i= 0; i < condf->argument_count(); i++) {
      if (xtrace)
        htrc("Argtype(%d)=%d\n", i, args[i]->type());

      if (i >= 2 && !ismul) {
        if (xtrace)
          htrc("Unexpected arg for vop=%d\n", vop);

        continue;
        } // endif i

      if ((iscol= args[i]->type() == COND::FIELD_ITEM)) {
        const char *fnm;
        ha_field_option_struct *fop;
        Item_field *pField= (Item_field *)args[i];

        if (x && i)
          return NULL;

        if (pField->field->table != table)
          return NULL;  // Field does not belong to this table
        else
          fop= GetFieldOptionStruct(pField->field);

        if (fop && fop->special) {
          if (tty == TYPE_AM_TBL && !stricmp(fop->special, "TABID"))
            fnm= "TABID";
          else if (tty == TYPE_AM_PLG)
            fnm= fop->special;
          else
            return NULL;

        } else if (tty == TYPE_AM_TBL)
          return NULL;
        else
          fnm= pField->field->field_name;

        if (xtrace) {
          htrc("Field index=%d\n", pField->field->field_index);
          htrc("Field name=%s\n", pField->field->field_name);
          } // endif xtrace

        // IN and BETWEEN clauses should be col VOP list
        if (i && ismul)
          return NULL;

        strcat(body, fnm);
      } else if (args[i]->type() == COND::FUNC_ITEM) {
        if (tty == TYPE_AM_MYSQL) {
          if (!CheckCond(g, filp, tty, args[i]))
            return NULL;

        } else
          return NULL;

      } else {
        char    buff[256];
        String *res, tmp(buff, sizeof(buff), &my_charset_bin);
        Item_basic_constant *pval= (Item_basic_constant *)args[i];

        switch (args[i]->real_type()) {
          case COND::STRING_ITEM:
          case COND::INT_ITEM:
          case COND::REAL_ITEM:
          case COND::NULL_ITEM:
          case COND::DECIMAL_ITEM:
          case COND::DATE_ITEM:
          case COND::CACHE_ITEM:
            break;
          default:
            return NULL;
          } // endswitch type

        if ((res= pval->val_str(&tmp)) == NULL)
          return NULL;                      // To be clarified

        if (xtrace)
          htrc("Value=%.*s\n", res->length(), res->ptr());

        // IN and BETWEEN clauses should be col VOP list
        if (!i && (x || ismul))
          return NULL;

        if (!x) {
          // Append the value to the filter
          if (args[i]->field_type() == MYSQL_TYPE_VARCHAR)
            strcat(strcat(strcat(body, "'"), res->ptr()), "'");
          else
            strncat(body, res->ptr(), res->length());

        } else {
          if (args[i]->field_type() == MYSQL_TYPE_VARCHAR) {
            // Add the command to the list
            PCMD *ncp, cmdp= new(g) CMD(g, (char*)res->ptr());

            for (ncp= &filp->Cmds; *ncp; ncp= &(*ncp)->Next) ;

            *ncp= cmdp;
          } else
            return NULL;

        } // endif x

      } // endif

      if (!x) {
        if (!i)
          strcat(body, GetValStr(vop, neg));
        else if (vop == OP_XX && i == 1)
          strcat(body, " AND ");
        else if (vop == OP_IN)
          strcat(body, (i == condf->argument_count() - 1) ? ")" : ",");

        } // endif x

      } // endfor i

    if (x)
      filp->Op= vop;

  } else {
    if (xtrace)
      htrc("Unsupported condition\n");

    return NULL;
  } // endif's type

  return filp;
} // end of CheckCond


 /**
   Push condition down to the table handler.

   @param  cond   Condition to be pushed. The condition tree must not be
                  modified by the caller.

   @return
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.

   @note
     CONNECT handles the filtering only for table types that construct
     an SQL or WQL query, but still leaves it to MySQL because only some
     parts of the filter may be relevant.
     The first suballocate finds the position where the string will be
     constructed in the sarea. The second one does make the suballocation
     with the proper length.
 */
const COND *ha_connect::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_connect::cond_push");

  if (tdbp) {
    PGLOBAL& g= xp->g;
    AMT      tty= tdbp->GetAmType();
    bool     x= (tty == TYPE_AM_MYX || tty == TYPE_AM_XDBC);
    bool     b= (tty == TYPE_AM_WMI || tty == TYPE_AM_ODBC  ||
                 tty == TYPE_AM_TBL || tty == TYPE_AM_MYSQL ||
                 tty == TYPE_AM_PLG || x);

    if (b) {
      PCFIL    filp= (PCFIL)PlugSubAlloc(g, NULL, sizeof(CONDFIL));

      filp->Body= (char*)PlugSubAlloc(g, NULL, (x) ? 128 : 0);
      *filp->Body= 0;
      filp->Op= OP_XX;
      filp->Cmds= NULL;

      if (CheckCond(g, filp, tty, (Item *)cond)) {
        if (xtrace)
          htrc("cond_push: %s\n", filp->Body);

        if (!x)
          PlugSubAlloc(g, NULL, strlen(filp->Body) + 1);
        else
          cond= NULL;             // Does this work?

        tdbp->SetCondFil(filp);
      } else if (x && cond)
        tdbp->SetCondFil(filp);   // Wrong filter

    } else
      tdbp->SetFilter(CondFilter(g, (Item *)cond));

    } // endif tdbp

  // Let MySQL do the filtering
  DBUG_RETURN(cond);
} // end of cond_push

/**
  Number of rows in table. It will only be called if
  (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
*/
ha_rows ha_connect::records()
{
  if (!valid_info)
    info(HA_STATUS_VARIABLE);

  if (tdbp)
    return stats.records;
  else
    return HA_POS_ERROR;

} // end of records


/**
  Return an error message specific to this handler.

  @param error  error code previously returned by handler
  @param buf    pointer to String where to add error message

  @return
    Returns true if this is a temporary error
*/
bool ha_connect::get_error_message(int error, String* buf)
{
  DBUG_ENTER("ha_connect::get_error_message");

  if (xp && xp->g) {
    PGLOBAL g= xp->g;
    char    msg[3072];         // MAX_STR * 3
    uint    dummy_errors;
    uint32  len= copy_and_convert(msg, strlen(g->Message) * 3,
                               system_charset_info,
                               g->Message, strlen(g->Message),
                               &my_charset_latin1,
                               &dummy_errors);

    if (trace)
      htrc("GEM(%u): %s\n", len, g->Message);

    msg[len]= '\0';
    buf->copy(msg, (uint)strlen(msg), system_charset_info);
  } else
    buf->copy("Cannot retrieve msg", 19, system_charset_info);

  DBUG_RETURN(false);
} // end of get_error_message

/**
  Convert a filename partition name to system
*/
static char *decode(PGLOBAL g, const char *pn)
  {
  char  *buf= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, strlen(pn) + 1,
                               system_charset_info,
                               pn, strlen(pn),
                               &my_charset_filename,
                               &dummy_errors);
  buf[len]= '\0';
  return buf;
  } // end of decode

/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @note
  For CONNECT no open can be done here because field information is not yet
  updated. >>>>> TO BE CHECKED <<<<<
  (Thread information could be get by using 'ha_thd')

  @see
  handler::ha_open() in handler.cc
*/
int ha_connect::open(const char *name, int mode, uint test_if_locked)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::open");

  if (xtrace)
     htrc("open: name=%s mode=%d test=%u\n", name, mode, test_if_locked);

  if (!(share= get_share()))
    DBUG_RETURN(1);

  thr_lock_data_init(&share->lock,&lock,NULL);

  // Try to get the user if possible
  xp= GetUser(ha_thd(), xp);
  PGLOBAL g= (xp) ? xp->g : NULL;

  // Try to set the database environment
  if (g) {
    rc= (CntCheckDB(g, this, name)) ? (-2) : 0;

    if (g->Mrr) {
      // This should only happen for the mrr secondary handler
      mrr= true;
      g->Mrr= false;
    } else
      mrr= false;

#if defined(WITH_PARTITION_STORAGE_ENGINE)
    if (table->part_info) {
      if (GetStringOption("Filename") || GetStringOption("Tabname")
          || GetStringOption("Connect")) {
        strcpy(partname, decode(g, strrchr(name, '#') + 1));
//      strcpy(partname, table->part_info->curr_part_elem->partition_name);
        part_id= &table->part_info->full_part_field_set;
      } else       // Inward table
        strcpy(partname, strrchr(name, slash) + 1);
        part_id= &table->part_info->full_part_field_set; // Temporary
      } // endif part_info
#endif   // WITH_PARTITION_STORAGE_ENGINE
  } else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of open

/**
  @brief
  Make the indexes for this table
*/
int ha_connect::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  PDBUSER  dup= PlgGetUser(g);

  // Ignore error on the opt file
  dup->Check &= ~CHK_OPT;
  tdbp= GetTDB(g);
  dup->Check |= CHK_OPT;

  if (tdbp) {
    bool dop= IsTypeIndexable(GetRealType(NULL));
    bool dox= (((PTDBASE)tdbp)->GetDef()->Indexable() == 1);

    if ((rc= ((PTDBASE)tdbp)->ResetTableOpt(g, dop, dox))) {
      if (rc == RC_INFO) {
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        rc= 0;
      } else
        rc= HA_ERR_INTERNAL_ERROR;

      } // endif rc

  } else
    rc= HA_ERR_INTERNAL_ERROR;

  return rc;
} // end of optimize

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/
int ha_connect::close(void)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::close");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
  if (tdbp && xp->last_query_id == valid_query_id)
    rc= CloseTable(xp->g);

  DBUG_RETURN(rc);
} // end of close


/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

    @details
  Example of this would be:
    @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
    @endcode

  See ha_tina.cc for an example of extracting all of the data as strings.
  ha_berekly.cc has an example of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments and timestamps. This
  case also applies to write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

    @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/
int ha_connect::write_row(uchar *buf)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::write_row");

  // This is not tested yet
  if (xmod == MODE_ALTER) {
    if (IsPartitioned() && GetStringOption("Filename", NULL))
      // Why does this happen now that check_if_supported_inplace_alter is called?
      DBUG_RETURN(0);     // Alter table on an outward partition table

    xmod= MODE_INSERT;
  } else if (xmod == MODE_ANY)
    DBUG_RETURN(0);       // Probably never met

  // Open the table if it was not opened yet (locked)
  if (!IsOpened() || xmod != tdbp->GetMode()) {
    if (IsOpened())
      CloseTable(g);

    if ((rc= OpenTable(g)))
      DBUG_RETURN(rc);

    } // endif isopened

#if 0                // AUTO_INCREMENT NIY
  if (table->next_number_field && buf == table->record[0]) {
    int error;

    if ((error= update_auto_increment()))
      return error;

    } // endif nex_number_field
#endif // 0

  // Set column values from the passed pseudo record
  if ((rc= ScanRecord(g, buf)))
    DBUG_RETURN(rc);

  // Return result code from write operation
  if (CntWriteRow(g, tdbp)) {
    DBUG_PRINT("write_row", ("%s", g->Message));
    htrc("write_row: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
  } else                // Table is modified
    nox= false;         // Indexes to be remade

  DBUG_RETURN(rc);
} // end of write_row


/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

    @details
  Currently new_data will not have an updated auto_increament record, or
  and updated timestamp field. You can do these for example by doing:
    @code
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
    table->timestamp_field->set_time();
  if (table->next_number_field && record == table->record[0])
    update_auto_increment();
    @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

    @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_connect::update_row(const uchar *old_data, uchar *new_data)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::update_row");

  if (xtrace > 1)
    htrc("update_row: old=%s new=%s\n", old_data, new_data);

  // Check values for possible change in indexed column
  if ((rc= CheckRecord(g, old_data, new_data)))
    DBUG_RETURN(rc);

  if (CntUpdateRow(g, tdbp)) {
    DBUG_PRINT("update_row", ("%s", g->Message));
    htrc("update_row CONNECT: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
  } else
    nox= false;               // Table is modified

  DBUG_RETURN(rc);
} // end of update_row


/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/
int ha_connect::delete_row(const uchar *buf)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::delete_row");

  if (CntDeleteRow(xp->g, tdbp, false)) {
    rc= HA_ERR_INTERNAL_ERROR;
    htrc("delete_row CONNECT: %s\n", xp->g->Message);
  } else
    nox= false;             // To remake indexes

  DBUG_RETURN(rc);
} // end of delete_row


/****************************************************************************/
/*  We seem to come here at the begining of an index use.                   */
/****************************************************************************/
int ha_connect::index_init(uint idx, bool sorted)
{
  int rc;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("index_init");

  if (xtrace)
    htrc("index_init: this=%p idx=%u sorted=%d\n", this, idx, sorted);

  if (GetIndexType(GetRealType()) == 2) {
    if (xmod == MODE_READ)
      // This is a remote index
      xmod= MODE_READX;

    if (!(rc= rnd_init(0))) {
//    if (xmod == MODE_READX) {
        active_index= idx;
        indexing= IsUnique(idx) ? 1 : 2;
//    } else {
//      active_index= MAX_KEY;
//      indexing= 0;
//    } // endif xmod

      } //endif rc

    DBUG_RETURN(rc);
    } // endif index type

  if ((rc= rnd_init(0)))
    DBUG_RETURN(rc);

  if (locked == 2) {
    // Indexes are not updated in lock write mode
    active_index= MAX_KEY;
    indexing= 0;
    DBUG_RETURN(0);
    } // endif locked

  indexing= CntIndexInit(g, tdbp, (signed)idx, sorted);

  if (indexing <= 0) {
    DBUG_PRINT("index_init", ("%s", g->Message));
    htrc("index_init CONNECT: %s\n", g->Message);
    active_index= MAX_KEY;
    rc= HA_ERR_INTERNAL_ERROR;
  } else if (((PTDBDOX)tdbp)->To_Kindex) {
    if (((PTDBDOX)tdbp)->To_Kindex->GetNum_K()) {
      if (((PTDBASE)tdbp)->GetFtype() != RECFM_NAF)
        ((PTDBDOX)tdbp)->GetTxfp()->ResetBuffer(g);

      active_index= idx;
//  } else {        // Void table
//    active_index= MAX_KEY;
//    indexing= 0;
    } // endif Num

    rc= 0;
  } // endif indexing

  if (xtrace)
    htrc("index_init: rc=%d indexing=%d active_index=%d\n",
            rc, indexing, active_index);

  DBUG_RETURN(rc);
} // end of index_init

/****************************************************************************/
/*  We seem to come here at the end of an index use.                        */
/****************************************************************************/
int ha_connect::index_end()
{
  DBUG_ENTER("index_end");
  active_index= MAX_KEY;
  ds_mrr.dsmrr_close();
  DBUG_RETURN(rnd_end());
} // end of index_end


/****************************************************************************/
/*  This is internally called by all indexed reading functions.             */
/****************************************************************************/
int ha_connect::ReadIndexed(uchar *buf, OPVAL op, const uchar *key, uint key_len)
{
  int rc;

//statistic_increment(ha_read_key_count, &LOCK_status);

  switch (CntIndexRead(xp->g, tdbp, op, key, (int)key_len, mrr)) {
    case RC_OK:
      xp->fnd++;
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      xp->nfd++;
      rc= (op == OP_SAME) ? HA_ERR_END_OF_FILE : HA_ERR_KEY_NOT_FOUND;
      break;
    default:          // Read error
      DBUG_PRINT("ReadIndexed", ("%s", xp->g->Message));
      htrc("ReadIndexed: %s\n", xp->g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
      break;
    } // endswitch RC

  if (xtrace > 1)
    htrc("ReadIndexed: op=%d rc=%d\n", op, rc);

  table->status= (rc == RC_OK) ? 0 : STATUS_NOT_FOUND;
  return rc;
} // end of ReadIndexed


#ifdef NOT_USED
/**
  @brief
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.
*/
int ha_connect::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map __attribute__((unused)),
                               enum ha_rkey_function find_flag
                               __attribute__((unused)))
{
  DBUG_ENTER("ha_connect::index_read");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/****************************************************************************/
/*  This is called by handler::index_read_map.                              */
/****************************************************************************/
int ha_connect::index_read(uchar * buf, const uchar * key, uint key_len,
                           enum ha_rkey_function find_flag)
{
  int rc;
  OPVAL op= OP_XX;
  DBUG_ENTER("ha_connect::index_read");

  switch(find_flag) {
    case HA_READ_KEY_EXACT:   op= OP_EQ; break;
    case HA_READ_AFTER_KEY:   op= OP_GT; break;
    case HA_READ_KEY_OR_NEXT: op= OP_GE; break;
    default: DBUG_RETURN(-1);      break;
    } // endswitch find_flag

  if (xtrace > 1)
    htrc("%p index_read: op=%d\n", this, op);

  if (indexing > 0) {
    rc= ReadIndexed(buf, op, key, key_len);

    if (rc == HA_ERR_INTERNAL_ERROR) {
      nox= true;                  // To block making indexes
      abort= true;                // Don't rename temp file
      } // endif rc

  } else
    rc= HA_ERR_INTERNAL_ERROR;  // HA_ERR_KEY_NOT_FOUND ?

  DBUG_RETURN(rc);
} // end of index_read


/**
  @brief
  Used to read forward through the index.
*/
int ha_connect::index_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next");
  //statistic_increment(ha_read_next_count, &LOCK_status);

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_NEXT);
  else if (!indexing)
    rc= rnd_next(buf);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next


/**
  @brief
  Used to read backwards through the index.
*/
int ha_connect::index_prev(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_prev");
  int rc;

  if (indexing > 0) {
    rc= ReadIndexed(buf, OP_PREV);
  } else
    rc= HA_ERR_WRONG_COMMAND;

  DBUG_RETURN(rc);
} // end of index_prev


/**
  @brief
  index_first() asks for the first key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_first(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_first");

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_FIRST);
  else if (indexing < 0)
    rc= HA_ERR_INTERNAL_ERROR;
  else if (CntRewindTable(xp->g, tdbp)) {
    table->status= STATUS_NOT_FOUND;
    rc= HA_ERR_INTERNAL_ERROR;
  } else
    rc= rnd_next(buf);

  DBUG_RETURN(rc);
} // end of index_first


/**
  @brief
  index_last() asks for the last key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_last(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_last");
  int rc;

  if (indexing <= 0) {
    rc= HA_ERR_INTERNAL_ERROR;
  } else
    rc= ReadIndexed(buf, OP_LAST);

  DBUG_RETURN(rc);
}


/****************************************************************************/
/*  This is called to get more rows having the same index value.            */
/****************************************************************************/
int ha_connect::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next_same");
//statistic_increment(ha_read_next_count, &LOCK_status);

  if (!indexing)
    rc= rnd_next(buf);
  else if (indexing > 0)
    rc= ReadIndexed(buf, OP_SAME);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next_same


/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the example in the introduction at the top of this file to see when
  rnd_init() is called.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @note
  We always call open and extern_lock/start_stmt before comming here.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_init(bool scan)
{
  PGLOBAL g= ((table && table->in_use) ? GetPlug(table->in_use, xp) :
              (xp) ? xp->g : NULL);
  DBUG_ENTER("ha_connect::rnd_init");

  // This is not tested yet
  if (xmod == MODE_ALTER) {
    xmod= MODE_READ;
    alter= 1;
    } // endif xmod

  if (xtrace)
    htrc("rnd_init: this=%p scan=%d xmod=%d alter=%d\n",
            this, scan, xmod, alter);

  if (!g || !table || xmod == MODE_INSERT)
    DBUG_RETURN(HA_ERR_INITIALIZATION);

  // Do not close the table if it was opened yet (locked?)
  if (IsOpened()) {
    if (IsPartitioned() && xmod != MODE_INSERT)
      if (CheckColumnList(g)) // map can have been changed
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

    if (tdbp->OpenDB(g))      // Rewind table
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    else
      DBUG_RETURN(0);

  } else if (xp->CheckQuery(valid_query_id))
    tdbp= NULL;       // Not valid anymore

  // When updating, to avoid skipped update, force the table
  // handler to retrieve write-only fields to be able to compare
  // records and detect data change.
  if (xmod == MODE_UPDATE)
    bitmap_union(table->read_set, table->write_set);

  if (OpenTable(g, xmod == MODE_DELETE))
    DBUG_RETURN(HA_ERR_INITIALIZATION);

  xp->nrd= xp->fnd= xp->nfd= 0;
  xp->tb1= my_interval_timer();
  DBUG_RETURN(0);
} // end of rnd_init

/**
  @brief
  Not described.

  @note
  The previous version said:
  Stop scanning of table. Note that this may be called several times during
  execution of a sub select.
  =====> This has been moved to external lock to avoid closing subselect tables.
*/
int ha_connect::rnd_end()
{
  int rc= 0;
  DBUG_ENTER("ha_connect::rnd_end");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
//  if (tdbp && xp->last_query_id == valid_query_id)
//    rc= CloseTable(xp->g);

  ds_mrr.dsmrr_close();
  DBUG_RETURN(rc);
} // end of rnd_end


/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::rnd_next");
//statistic_increment(ha_read_rnd_next_count, &LOCK_status);

  if (tdbp->GetMode() == MODE_ANY) {
    // We will stop on next read
    if (!stop) {
      stop= true;
      DBUG_RETURN(RC_OK);
    } else
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    } // endif Mode

  switch (CntReadNext(xp->g, tdbp)) {
    case RC_OK:
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      rc= HA_ERR_RECORD_DELETED;
      break;
    default:            // Read error
      htrc("rnd_next CONNECT: %s\n", xp->g->Message);
      rc= (records()) ? HA_ERR_INTERNAL_ERROR : HA_ERR_END_OF_FILE;
      break;
    } // endswitch RC

  if (xtrace > 1 && (rc || !(xp->nrd++ % 16384))) {
    ulonglong tb2= my_interval_timer();
    double elapsed= (double) (tb2 - xp->tb1) / 1000000000ULL;
    DBUG_PRINT("rnd_next", ("rc=%d nrd=%u fnd=%u nfd=%u sec=%.3lf\n",
                             rc, (uint)xp->nrd, (uint)xp->fnd,
                             (uint)xp->nfd, elapsed));
    htrc("rnd_next: rc=%d nrd=%u fnd=%u nfd=%u sec=%.3lf\n",
                             rc, (uint)xp->nrd, (uint)xp->fnd,
                             (uint)xp->nfd, elapsed);
    xp->tb1= tb2;
    xp->fnd= xp->nfd= 0;
    } // endif nrd

  table->status= (!rc) ? 0 : STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
} // end of rnd_next


/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
    @code
  my_store_ptr(ref, ref_length, current_position);
    @endcode

    @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

    @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_connect::position(const uchar *record)
{
  DBUG_ENTER("ha_connect::position");
//if (((PTDBASE)tdbp)->GetDef()->Indexable())
    my_store_ptr(ref, ref_length, (my_off_t)((PTDBASE)tdbp)->GetRecpos());

  if (trace)
    htrc("position: pos=%d\n", ((PTDBASE)tdbp)->GetRecpos());

  DBUG_VOID_RETURN;
} // end of position


/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use my_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

    @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

    @note
  Is this really useful? It was never called even when sorting.

    @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_connect::rnd_pos(uchar *buf, uchar *pos)
{
  int     rc;
  PTDBASE tp= (PTDBASE)tdbp;
  DBUG_ENTER("ha_connect::rnd_pos");

  if (!tp->SetRecpos(xp->g, (int)my_get_ptr(pos, ref_length))) {
    if (trace)
      htrc("rnd_pos: %d\n", tp->GetRecpos());

    tp->SetFilter(NULL);
    rc= rnd_next(buf);
  } else
    rc= HA_ERR_KEY_NOT_FOUND;

  DBUG_RETURN(rc);
} // end of rnd_pos


/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

    @details
  Currently this table handler doesn't implement most of the fields really needed.
  SHOW also makes use of this data.

  You will probably want to have the following in your code:
    @code
  if (records < 2)
    records= 2;
    @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_table.cc, sql_union.cc, and sql_update.cc.

    @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
  sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
  sql_union.cc and sql_update.cc
*/
int ha_connect::info(uint flag)
{
  bool    pure= false;
  PGLOBAL g= GetPlug((table) ? table->in_use : NULL, xp);

  DBUG_ENTER("ha_connect::info");

  if (xtrace)
    htrc("%p In info: flag=%u valid_info=%d\n", this, flag, valid_info);

  // tdbp must be available to get updated info
  if (xp->CheckQuery(valid_query_id) || !tdbp) {
    PDBUSER dup= PlgGetUser(g);
    PCATLG  cat= (dup) ? dup->Catalog : NULL;

    if (xmod == MODE_ANY || xmod == MODE_ALTER) {
      // Pure info, not a query
      pure= true;
      xp->CheckCleanup();
      } // endif xmod

    // This is necessary for getting file length
//  if (cat && table)
//    cat->SetDataPath(g, table->s->db.str);
    if (table)
      SetDataPath(g, table->s->db.str);
    else
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);       // Should never happen

    if (!(tdbp= GetTDB(g)))
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);       // Should never happen

    valid_info = false;
    } // endif tdbp

  if (!valid_info) {
    valid_info= CntInfo(g, tdbp, &xinfo);

    if (((signed)xinfo.records) < 0)
      DBUG_RETURN(HA_ERR_INITIALIZATION);  // Error in Cardinality

    } // endif valid_info

  if (flag & HA_STATUS_VARIABLE) {
    stats.records= xinfo.records;
    stats.deleted= 0;
    stats.data_file_length= xinfo.data_file_length;
    stats.index_file_length= 0;
    stats.delete_length= 0;
    stats.check_time= 0;
    stats.mean_rec_length= xinfo.mean_rec_length;
    } // endif HA_STATUS_VARIABLE

  if (flag & HA_STATUS_CONST) {
    // This is imported from the previous handler and must be reconsidered
    stats.max_data_file_length= 4294967295LL;
    stats.max_index_file_length= 4398046510080LL;
    stats.create_time= 0;
    data_file_name= xinfo.data_file_name;
    index_file_name= NULL;
//  sortkey= (uint) - 1;           // Table is not sorted
    ref_length= sizeof(int);      // Pointer size to row
    table->s->db_options_in_use= 03;
    stats.block_size= 1024;
    table->s->keys_in_use.set_prefix(table->s->keys);
    table->s->keys_for_keyread= table->s->keys_in_use;
//  table->s->keys_for_keyread.subtract(table->s->read_only_keys);
    table->s->db_record_offset= 0;
    } // endif HA_STATUS_CONST

  if (flag & HA_STATUS_ERRKEY) {
    errkey= 0;
    } // endif HA_STATUS_ERRKEY

  if (flag & HA_STATUS_TIME)
    stats.update_time= 0;

  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= 1;

  if (tdbp && pure)
    CloseTable(g);        // Not used anymore

  DBUG_RETURN(0);
} // end of info


/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

  @note
  This is not yet implemented for CONNECT.

  @see
  ha_innodb.cc
*/
int ha_connect::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_connect::extra");
  DBUG_RETURN(0);
} // end of extra


/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases where
  the optimizer realizes that all rows will be removed as a result of an SQL statement.

    @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

    @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_connect::delete_all_rows()
{
  int     rc= 0;
  PGLOBAL g= xp->g;
  DBUG_ENTER("ha_connect::delete_all_rows");

  if (tdbp && tdbp->GetUse() == USE_OPEN &&
      tdbp->GetAmType() != TYPE_AM_XML &&
      ((PTDBASE)tdbp)->GetFtype() != RECFM_NAF)
    // Close and reopen the table so it will be deleted
    rc= CloseTable(g);

  if (!(rc= OpenTable(g))) {
    if (CntDeleteRow(g, tdbp, true)) {
      htrc("%s\n", g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
    } else
      nox= false;

    } // endif rc

  DBUG_RETURN(rc);
} // end of delete_all_rows


bool ha_connect::check_privileges(THD *thd, PTOS options, char *dbn)
{
  const char *db= (dbn && *dbn) ? dbn : NULL;
  TABTYPE     type=GetRealType(options);

  switch (type) {
    case TAB_UNDEF:
//  case TAB_CATLG:
    case TAB_PLG:
    case TAB_JCT:
    case TAB_DMY:
    case TAB_NIY:
      my_printf_error(ER_UNKNOWN_ERROR,
                      "Unsupported table type %s", MYF(0), options->type);
      return true;

    case TAB_DOS:
    case TAB_FIX:
    case TAB_BIN:
    case TAB_CSV:
    case TAB_FMT:
    case TAB_DBF:
    case TAB_XML:
    case TAB_INI:
    case TAB_VEC:
      if (options->filename && *options->filename) {
        char *s, path[FN_REFLEN], dbpath[FN_REFLEN];
#if defined(WIN32)
        s= "\\";
#else   // !WIN32
        s= "/";
#endif  // !WIN32
        strcpy(dbpath, mysql_real_data_home);

        if (db)
          strcat(strcat(dbpath, db), s);

        (void) fn_format(path, options->filename, dbpath, "",
                         MY_RELATIVE_PATH | MY_UNPACK_FILENAME);

        if (!is_secure_file_path(path)) {
          my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
          return true;
          } // endif path

      } else
        return false;

      /* Fall through to check FILE_ACL */
    case TAB_ODBC:
    case TAB_MYSQL:
    case TAB_DIR:
    case TAB_MAC:
    case TAB_WMI:
    case TAB_OEM:
      return check_access(thd, FILE_ACL, db, NULL, NULL, 0, 0);

    // This is temporary until a solution is found
    case TAB_TBL:
    case TAB_XCL:
    case TAB_PRX:
    case TAB_OCCUR:
    case TAB_PIVOT:
      return false;
    } // endswitch type

  my_printf_error(ER_UNKNOWN_ERROR, "check_privileges failed", MYF(0));
  return true;
} // end of check_privileges

// Check that two indexes are equivalent
bool ha_connect::IsSameIndex(PIXDEF xp1, PIXDEF xp2)
{
  bool   b= true;
  PKPDEF kp1, kp2;

  if (stricmp(xp1->Name, xp2->Name))
    b= false;
  else if (xp1->Nparts  != xp2->Nparts  ||
           xp1->MaxSame != xp2->MaxSame ||
           xp1->Unique  != xp2->Unique)
    b= false;
  else for (kp1= xp1->ToKeyParts, kp2= xp2->ToKeyParts;
            b && (kp1 || kp2);
            kp1= kp1->Next, kp2= kp2->Next)
    if (!kp1 || !kp2)
      b= false;
    else if (stricmp(kp1->Name, kp2->Name))
      b= false;
    else if (kp1->Klen != kp2->Klen)
      b= false;

  return b;
} // end of IsSameIndex

MODE ha_connect::CheckMode(PGLOBAL g, THD *thd,
                           MODE newmode, bool *chk, bool *cras)
{
  if ((trace= xtrace)) {
    LEX_STRING *query_string= thd_query_string(thd);
    htrc("%p check_mode: cmdtype=%d\n", this, thd_sql_command(thd));
    htrc("Cmd=%.*s\n", (int) query_string->length, query_string->str);
    } // endif xtrace

  // Next code is temporarily replaced until sql_command is set
  stop= false;

  if (newmode == MODE_WRITE) {
    switch (thd_sql_command(thd)) {
      case SQLCOM_LOCK_TABLES:
        locked= 2;
      case SQLCOM_CREATE_TABLE:
      case SQLCOM_INSERT:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
        newmode= MODE_INSERT;
        break;
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
//      newmode= MODE_UPDATE;               // To be checked
//      break;
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_TRUNCATE:
        newmode= MODE_DELETE;
        break;
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
        newmode= MODE_UPDATE;
        break;
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        newmode= MODE_READ;
        break;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
        newmode= MODE_ANY;
        break;
      case SQLCOM_CREATE_VIEW:
      case SQLCOM_DROP_VIEW:
        newmode= MODE_ANY;
        break;
      case SQLCOM_ALTER_TABLE:
        newmode= MODE_ALTER;
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
//      if (!IsPartitioned()) {
          newmode= MODE_ANY;
          break;
//        } // endif partitioned

      default:
        htrc("Unsupported sql_command=%d\n", thd_sql_command(thd));
        strcpy(g->Message, "CONNECT Unsupported command");
        my_message(ER_NOT_ALLOWED_COMMAND, g->Message, MYF(0));
        newmode= MODE_ERROR;
        break;
      } // endswitch newmode

  } else if (newmode == MODE_READ) {
    switch (thd_sql_command(thd)) {
      case SQLCOM_CREATE_TABLE:
        *chk= true;
        *cras= true;
      case SQLCOM_INSERT:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_TRUNCATE:
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        break;
      case SQLCOM_LOCK_TABLES:
        locked= 1;
        break;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
        newmode= MODE_ANY;
        break;
      case SQLCOM_CREATE_VIEW:
      case SQLCOM_DROP_VIEW:
        newmode= MODE_ANY;
        break;
      case SQLCOM_ALTER_TABLE:
        *chk= true;
        newmode= MODE_ALTER;
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
//      if (!IsPartitioned()) {
          *chk= true;
          newmode= MODE_ANY;
          break;
//        } // endif partitioned

      default:
        htrc("Unsupported sql_command=%d\n", thd_sql_command(thd));
        strcpy(g->Message, "CONNECT Unsupported command");
        my_message(ER_NOT_ALLOWED_COMMAND, g->Message, MYF(0));
        newmode= MODE_ERROR;
        break;
      } // endswitch newmode

  } // endif's newmode

  if (xtrace)
    htrc("New mode=%d\n", newmode);

  return newmode;
} // end of check_mode

int ha_connect::start_stmt(THD *thd, thr_lock_type lock_type)
{
  int     rc= 0;
  bool    chk=false, cras= false;
  MODE    newmode;
  PGLOBAL g= GetPlug(thd, xp);
  DBUG_ENTER("ha_connect::start_stmt");

  // Action will depend on lock_type
  switch (lock_type) {
    case TL_WRITE_ALLOW_WRITE:
    case TL_WRITE_CONCURRENT_INSERT:
    case TL_WRITE_DELAYED:
    case TL_WRITE_DEFAULT:
    case TL_WRITE_LOW_PRIORITY:
    case TL_WRITE:
    case TL_WRITE_ONLY:
      newmode= MODE_WRITE;
      break;
    case TL_READ:
    case TL_READ_WITH_SHARED_LOCKS:
    case TL_READ_HIGH_PRIORITY:
    case TL_READ_NO_INSERT:
    case TL_READ_DEFAULT:
      newmode= MODE_READ;
      break;
    case TL_UNLOCK:
    default:
      newmode= MODE_ANY;
      break;
    } // endswitch mode

  xmod= CheckMode(g, thd, newmode, &chk, &cras);
  DBUG_RETURN((xmod == MODE_ERROR) ? HA_ERR_INTERNAL_ERROR : 0);
} // end of start_stmt

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
  this.

    @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

    @note
  Following what we did in the MySQL XDB handler, we use this call to actually
  physically open the table. This could be reconsider when finalizing this handler
  design, which means we have a better understanding of what MariaDB does.

    @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_connect::external_lock(THD *thd, int lock_type)
{
  int     rc= 0;
  bool    xcheck=false, cras= false;
  MODE    newmode;
  PTOS    options= GetTableOptionStruct();
  PGLOBAL g= GetPlug(thd, xp);
  DBUG_ENTER("ha_connect::external_lock");

  DBUG_ASSERT(thd == current_thd);

  if (xtrace)
    htrc("external_lock: this=%p thd=%p xp=%p g=%p lock_type=%d\n",
            this, thd, xp, g, lock_type);

  if (!g)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // Action will depend on lock_type
  switch (lock_type) {
    case F_WRLCK:
      newmode= MODE_WRITE;
      break;
    case F_RDLCK:
      newmode= MODE_READ;
      break;
    case F_UNLCK:
    default:
      newmode= MODE_ANY;
      break;
    } // endswitch mode

  if (newmode == MODE_ANY) {
    int sqlcom= thd_sql_command(thd);

    // This is unlocking, do it by closing the table
    if (xp->CheckQueryID() && sqlcom != SQLCOM_UNLOCK_TABLES
                           && sqlcom != SQLCOM_LOCK_TABLES
                           && sqlcom != SQLCOM_DROP_TABLE) {
      sprintf(g->Message, "external_lock: unexpected command %d", sqlcom);
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
      DBUG_RETURN(0);
    } else if (g->Xchk) {
      if (!tdbp) {
        if (!(tdbp= GetTDB(g)))
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
        else if (!((PTDBASE)tdbp)->GetDef()->Indexable()) {
          sprintf(g->Message, "external_lock: Table %s is not indexable", tdbp->GetName());
//        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);  causes assert error
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
          DBUG_RETURN(0);
        } else if (((PTDBASE)tdbp)->GetDef()->Indexable() == 1) {
          bool    oldsep= ((PCHK)g->Xchk)->oldsep;
          bool    newsep= ((PCHK)g->Xchk)->newsep;
          PTDBDOS tdp= (PTDBDOS)tdbp;
      
          PDOSDEF ddp= (PDOSDEF)tdp->GetDef();
          PIXDEF  xp, xp1, xp2, drp=NULL, adp= NULL;
          PIXDEF  oldpix= ((PCHK)g->Xchk)->oldpix;
          PIXDEF  newpix= ((PCHK)g->Xchk)->newpix;
          PIXDEF *xlst, *xprc; 
      
          ddp->SetIndx(oldpix);
      
          if (oldsep != newsep) {
            // All indexes have to be remade
            ddp->DeleteIndexFile(g, NULL);
            oldpix= NULL;
            ddp->SetIndx(NULL);
            SetBooleanOption("Sepindex", newsep);
          } else if (newsep) {
            // Make the list of dropped indexes
            xlst= &drp; xprc= &oldpix;
      
            for (xp2= oldpix; xp2; xp2= xp) {
              for (xp1= newpix; xp1; xp1= xp1->Next)
                if (IsSameIndex(xp1, xp2))
                  break;        // Index not to drop
      
              xp= xp2->GetNext();
      
              if (!xp1) {
                *xlst= xp2;
                *xprc= xp;
                *(xlst= &xp2->Next)= NULL;
              } else
                xprc= &xp2->Next;
      
              } // endfor xp2
      
            if (drp) {
              // Here we erase the index files
              ddp->DeleteIndexFile(g, drp);
              } // endif xp1
      
          } else if (oldpix) {
            // TODO: optimize the case of just adding new indexes
            if (!newpix)
              ddp->DeleteIndexFile(g, NULL);
      
            oldpix= NULL;     // To remake all indexes
            ddp->SetIndx(NULL);
          } // endif sepindex
      
          // Make the list of new created indexes
          xlst= &adp; xprc= &newpix;
      
          for (xp1= newpix; xp1; xp1= xp) {
            for (xp2= oldpix; xp2; xp2= xp2->Next)
              if (IsSameIndex(xp1, xp2))
                break;        // Index already made
      
            xp= xp1->Next;
      
            if (!xp2) {
              *xlst= xp1;
              *xprc= xp;
              *(xlst= &xp1->Next)= NULL;
            } else
              xprc= &xp1->Next;
      
            } // endfor xp1
      
          if (adp)
            // Here we do make the new indexes
            if (tdp->MakeIndex(g, adp, true) == RC_FX) {
              // Make it a warning to avoid crash
              push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 
                                0, g->Message);
              rc= 0;
              } // endif MakeIndex
      
        } // endif indexable

        } // endif Tdbp

      } // endelse Xchk

    if (CloseTable(g)) {
      // This is an error while builing index
      // Make it a warning to avoid crash
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
      rc= 0;
      } // endif Close

    locked= 0;
    xmod= MODE_ANY;              // For info commands
    DBUG_RETURN(rc);
    } // endif MODE_ANY

  DBUG_ASSERT(table && table->s);

  if (check_privileges(thd, options, table->s->db.str)) {
    strcpy(g->Message, "This operation requires the FILE privilege");
    htrc("%s\n", g->Message);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    } // endif check_privileges

  // Table mode depends on the query type
  newmode= CheckMode(g, thd, newmode, &xcheck, &cras);

  if (newmode == MODE_ERROR)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // If this is the start of a new query, cleanup the previous one
  if (xp->CheckCleanup()) {
    tdbp= NULL;
    valid_info= false;
    } // endif CheckCleanup

#if 0
  if (xcheck) {
    // This must occur after CheckCleanup
    if (!g->Xchk) {
      g->Xchk= new(g) XCHK;
      ((PCHK)g->Xchk)->oldsep= GetBooleanOption("Sepindex", false);
      ((PCHK)g->Xchk)->oldpix= GetIndexInfo();
      } // endif Xchk

  } else
    g->Xchk= NULL;
#endif // 0

  if (cras)
    g->Createas= 1;       // To tell created table to ignore FLAG

  if (xtrace) {
#if 0
    htrc("xcheck=%d cras=%d\n", xcheck, cras);

    if (xcheck)
      htrc("oldsep=%d oldpix=%p\n",
              ((PCHK)g->Xchk)->oldsep, ((PCHK)g->Xchk)->oldpix);
#endif // 0
    htrc("Calling CntCheckDB db=%s cras=%d\n", GetDBName(NULL), cras);
    } // endif xtrace

  // Set or reset the good database environment
  if (CntCheckDB(g, this, GetDBName(NULL))) {
    htrc("%p external_lock: %s\n", this, g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
  // This can NOT be called without open called first, but
  // the table can have been closed since then
  } else if (!tdbp || xp->CheckQuery(valid_query_id) || xmod != newmode) {
    if (tdbp) {
      // If this is called by a later query, the table may have
      // been already closed and the tdbp is not valid anymore.
      if (xp->last_query_id == valid_query_id)
        rc= CloseTable(g);
      else
        tdbp= NULL;

      } // endif tdbp

    xmod= newmode;

    // Delay open until used fields are known
  } // endif tdbp

  if (xtrace)
    htrc("external_lock: rc=%d\n", rc);

  DBUG_RETURN(rc);
} // end of external_lock


/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

    @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

    @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

    @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_connect::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type=lock_type;
  *to++ = &lock;
  return to;
}


/**
  Searches for a pointer to the last occurrence of  the
  character c in the string src.
  Returns true on failure, false on success.
*/
static bool
strnrchr(LEX_CSTRING *ls, const char *src, size_t length, int c)
{
  const char *srcend, *s;
  for (s= srcend= src + length; s > src; s--)
  {
    if (s[-1] == c)
    {
      ls->str= s;
      ls->length= srcend - s;
      return false;
    }
  }
  return true;
}


/**
  Split filename into database and table name.
*/
static bool
filename_to_dbname_and_tablename(const char *filename,
                                 char *database, size_t database_size,
                                 char *table, size_t table_size)
{
  LEX_CSTRING d, t;
  size_t length= strlen(filename);

  /* Find filename - the rightmost directory part */
  if (strnrchr(&t, filename, length, slash) || t.length + 1 > table_size)
    return true;
  memcpy(table, t.str, t.length);
  table[t.length]= '\0';
  if (!(length-= t.length))
    return true;

  length--; /* Skip slash */

  /* Find database name - the second rightmost directory part */
  if (strnrchr(&d, filename, length, slash) || d.length + 1 > database_size)
    return true;
  memcpy(database, d.str, d.length);
  database[d.length]= '\0';
  return false;
} // end of filename_to_dbname_and_tablename

/**
  @brief
  Used to delete or rename a table. By the time delete_table() has been
  called all opened references to this table will have been closed
  (and your globally shared references released) ===> too bad!!!
  The variable name will just be the name of the table.
  You will need to remove or rename any files you have created at
  this point.

    @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions returned
  by bas_ext().

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

    @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_connect::delete_or_rename_table(const char *name, const char *to)
{
  DBUG_ENTER("ha_connect::delete_or_rename_table");
  char db[128], tabname[128];
  int  rc= 0;
  bool ok= false;
  THD *thd= current_thd;
  int  sqlcom= thd_sql_command(thd);

  if (xtrace) {
    if (to)
      htrc("rename_table: this=%p thd=%p sqlcom=%d from=%s to=%s\n",
              this, thd, sqlcom, name, to);
    else
      htrc("delete_table: this=%p thd=%p sqlcom=%d name=%s\n",
              this, thd, sqlcom, name);

    } // endif xtrace

  if (to && (filename_to_dbname_and_tablename(to, db, sizeof(db),
                                             tabname, sizeof(tabname))
      || (*tabname == '#' && sqlcom == SQLCOM_CREATE_INDEX)))
    DBUG_RETURN(0);

  if (filename_to_dbname_and_tablename(name, db, sizeof(db),
                                       tabname, sizeof(tabname))
      || (*tabname == '#' && sqlcom == SQLCOM_CREATE_INDEX))
    DBUG_RETURN(0);

  // If a temporary file exists, all the tests below were passed
  // successfully when making it, so they are not needed anymore
  // in particular because they sometimes cause DBUG_ASSERT crash.
  // Also, for partitioned tables, no test can be done because when
  // this function is called, the .par file is already deleted and
  // this causes the open_table_def function to fail.
  // Not having any other clues (table and table_share are NULL)
  // the only mean we have to test for partitioning is this:
  if (*tabname != '#' && !strstr(tabname, "#P#")) {
    // We have to retrieve the information about this table options.
    ha_table_option_struct *pos;
    char         key[MAX_DBKEY_LENGTH];
    uint         key_length;
    TABLE_SHARE *share;

//  if ((p= strstr(tabname, "#P#")))   won't work, see above
//    *p= 0;             // Get the main the table name

    key_length= tdc_create_key(key, db, tabname);

    // share contains the option struct that we need
    if (!(share= alloc_table_share(db, tabname, key, key_length)))
      DBUG_RETURN(rc);

    // Get the share info from the .frm file
    if (!open_table_def(thd, share)) {
      // Now we can work
      if ((pos= share->option_struct)) {
        if (check_privileges(thd, pos, db))
          rc= HA_ERR_INTERNAL_ERROR;         // ???
        else
          if (IsFileType(GetRealType(pos)) && !pos->filename)
            ok= true;

        } // endif pos

    } else       // Avoid infamous DBUG_ASSERT
      thd->get_stmt_da()->reset_diagnostics_area();

    free_table_share(share);
  } else              // Temporary file
    ok= true;

  if (ok) {
    // Let the base handler do the job
    if (to)
      rc= handler::rename_table(name, to);
    else if ((rc= handler::delete_table(name)) == ENOENT)
      rc= 0;        // No files is not an error for CONNECT

    } // endif ok

  DBUG_RETURN(rc);
} // end of delete_or_rename_table

int ha_connect::delete_table(const char *name)
{
  return delete_or_rename_table(name, NULL);
} // end of delete_table

int ha_connect::rename_table(const char *from, const char *to)
{
  return delete_or_rename_table(from, to);
} // end of rename_table

/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_connect::records_in_range(uint inx, key_range *min_key,
                                               key_range *max_key)
{
  ha_rows rows;
  DBUG_ENTER("ha_connect::records_in_range");

  if (indexing < 0 || inx != active_index)
    if (index_init(inx, false))
      DBUG_RETURN(HA_POS_ERROR);

  if (xtrace)
    htrc("records_in_range: inx=%d indexing=%d\n", inx, indexing);

  if (indexing > 0) {
    int          nval;
    uint         len[2];
    const uchar *key[2];
    bool         incl[2];
    key_part_map kmap[2];

    key[0]= (min_key) ? min_key->key : NULL;
    key[1]= (max_key) ? max_key->key : NULL;
    len[0]= (min_key) ? min_key->length : 0;
    len[1]= (max_key) ? max_key->length : 0;
    incl[0]= (min_key) ? (min_key->flag == HA_READ_KEY_EXACT) : false;
    incl[1]= (max_key) ? (max_key->flag == HA_READ_AFTER_KEY) : false;
    kmap[0]= (min_key) ? min_key->keypart_map : 0;
    kmap[1]= (max_key) ? max_key->keypart_map : 0;

    if ((nval= CntIndexRange(xp->g, tdbp, key, len, incl, kmap)) < 0)
      rows= HA_POS_ERROR;
    else
      rows= (ha_rows)nval;

  } else if (indexing == 0)
    rows= 100000000;        // Don't use missing index
  else
    rows= HA_POS_ERROR;

  DBUG_RETURN(rows);
} // end of records_in_range

/**
  Convert an ISO-8859-1 column name to UTF-8
*/
static char *encode(PGLOBAL g, const char *cnm)
  {
  char  *buf= (char*)PlugSubAlloc(g, NULL, strlen(cnm) * 3);
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, strlen(cnm) * 3,
                               &my_charset_utf8_general_ci,
                               cnm, strlen(cnm),
                               &my_charset_latin1,
                               &dummy_errors);
  buf[len]= '\0';
  return buf;
  } // end of encode

/**
  Store field definition for create.

  @return
    Return 0 if ok
*/
#if defined(NEW_WAY)
static bool add_fields(PGLOBAL g,
                       THD *thd,
                       Alter_info *alter_info,
                       char *name,
                       int typ, int len, int dec,
                       uint type_modifier,
                       char *rem,
//                     CHARSET_INFO *cs,
//                     void *vcolinfo,
//                     engine_option_value *create_options,
                       int flg,
                       bool dbf,
                       char v)
{
  register Create_field *new_field;
  char *length, *decimals= NULL;
  enum_field_types type;
//Virtual_column_info *vcol_info= (Virtual_column_info *)vcolinfo;
  engine_option_value *crop;
  LEX_STRING *comment;
  LEX_STRING *field_name;

  DBUG_ENTER("ha_connect::add_fields");

  if (len) {
    if (!v && typ == TYPE_STRING && len > 255)
      v= 'V';     // Change CHAR to VARCHAR

    length= (char*)PlugSubAlloc(g, NULL, 8);
    sprintf(length, "%d", len);

    if (typ == TYPE_DOUBLE) {
      decimals= (char*)PlugSubAlloc(g, NULL, 8);
      sprintf(decimals, "%d", min(dec, (min(len, 31) - 1)));
      } // endif dec

  } else
    length= NULL;

  if (!rem)
    rem= "";

  type= PLGtoMYSQL(typ, dbf, v);
  comment= thd->make_lex_string(rem, strlen(rem));
  field_name= thd->make_lex_string(name, strlen(name));

  switch (v) {
    case 'Z': type_modifier|= ZEROFILL_FLAG;
    case 'U': type_modifier|= UNSIGNED_FLAG; break;
    } // endswitch v

  if (flg) {
    engine_option_value *start= NULL, *end= NULL;
    LEX_STRING *flag= thd->make_lex_string("flag", 4);

    crop= new(thd->mem_root) engine_option_value(*flag, (ulonglong)flg,
                                                 &start, &end, thd->mem_root);
  } else
    crop= NULL;

  if (check_string_char_length(field_name, "", NAME_CHAR_LEN,
                               system_charset_info, 1)) {
    my_error(ER_TOO_LONG_IDENT, MYF(0), field_name->str); /* purecov: inspected */
    DBUG_RETURN(1);       /* purecov: inspected */
    } // endif field_name

  if (!(new_field= new Create_field()) ||
        new_field->init(thd, field_name->str, type, length, decimals,
                        type_modifier, NULL, NULL, comment, NULL,
                        NULL, NULL, 0, NULL, crop, true))
    DBUG_RETURN(1);

  alter_info->create_list.push_back(new_field);
  DBUG_RETURN(0);
} // end of add_fields
#else   // !NEW_WAY
static bool add_field(String *sql, const char *field_name, int typ,
                      int len, int dec, uint tm, const char *rem,
                      char *dft, char *xtra, int flag, bool dbf, char v)
{
  char var = (len > 255) ? 'V' : v;
  bool error= false;
  const char *type= PLGtoMYSQLtype(typ, dbf, var);

  error|= sql->append('`');
  error|= sql->append(field_name);
  error|= sql->append("` ");
  error|= sql->append(type);

  if (len && typ != TYPE_DATE) {
    error|= sql->append('(');
    error|= sql->append_ulonglong(len);

    if (!strcmp(type, "DOUBLE")) {
      error|= sql->append(',');
      // dec must be < len and < 31
      error|= sql->append_ulonglong(MY_MIN(dec, (MY_MIN(len, 31) - 1)));
    } else if (dec > 0 && !strcmp(type, "DECIMAL")) {
      error|= sql->append(',');
      // dec must be < len
      error|= sql->append_ulonglong(MY_MIN(dec, len - 1));
    } // endif dec

    error|= sql->append(')');
    } // endif len

  if (v == 'U')
    error|= sql->append(" UNSIGNED");
  else if (v == 'Z')
    error|= sql->append(" ZEROFILL");

  if (tm)
    error|= sql->append(STRING_WITH_LEN(" NOT NULL"), system_charset_info);

  if (dft && *dft) {
    error|= sql->append(" DEFAULT ");

    if (!IsTypeNum(typ)) {
      error|= sql->append("'");
      error|= sql->append_for_single_quote(dft, strlen(dft));
      error|= sql->append("'");
    } else
      error|= sql->append(dft);

    } // endif dft

  if (xtra && *xtra) {
    error|= sql->append(" ");
    error|= sql->append(xtra);
    } // endif rem

  if (rem && *rem) {
    error|= sql->append(" COMMENT '");
    error|= sql->append_for_single_quote(rem, strlen(rem));
    error|= sql->append("'");
    } // endif rem

  if (flag) {
    error|= sql->append(" FLAG=");
    error|= sql->append_ulonglong(flag);
    } // endif flag

  error|= sql->append(',');
  return error;
} // end of add_field
#endif  // !NEW_WAY

/**
  Initialise the table share with the new columns.

  @return
    Return 0 if ok
*/
#if defined(NEW_WAY)
//static bool sql_unusable_for_discovery(THD *thd, const char *sql);

static int init_table_share(THD *thd,
                            TABLE_SHARE *table_s,
                            HA_CREATE_INFO *create_info,
                            Alter_info *alter_info)
{
  KEY         *not_used_1;
  uint         not_used_2;
  int          rc= 0;
  handler     *file;
  LEX_CUSTRING frm= {0,0};

  DBUG_ENTER("init_table_share");

#if 0
  ulonglong saved_mode= thd->variables.sql_mode;
  CHARSET_INFO *old_cs= thd->variables.character_set_client;
  Parser_state parser_state;
  char *sql_copy;
  LEX *old_lex;
  Query_arena *arena, backup;
  LEX tmp_lex;

  /*
    Ouch. Parser may *change* the string it's working on.
    Currently (2013-02-26) it is used to permanently disable
    conditional comments.
    Anyway, let's copy the caller's string...
  */
  if (!(sql_copy= thd->strmake(sql, sql_length)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  if (parser_state.init(thd, sql_copy, sql_length))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  thd->variables.sql_mode= MODE_NO_ENGINE_SUBSTITUTION | MODE_NO_DIR_IN_CREATE;
  thd->variables.character_set_client= system_charset_info;
  old_lex= thd->lex;
  thd->lex= &tmp_lex;

  arena= thd->stmt_arena;

  if (arena->is_conventional())
    arena= 0;
  else
    thd->set_n_backup_active_arena(arena, &backup);

  lex_start(thd);

  if ((error= parse_sql(thd, & parser_state, NULL)))
    goto ret;

  if (table_s->sql_unusable_for_discovery(thd, NULL)) {
    my_error(ER_SQL_DISCOVER_ERROR, MYF(0), plugin_name(db_plugin)->str,
             db.str, table_name.str, sql_copy);
    goto ret;
    } // endif unusable

  thd->lex->create_info.db_type= plugin_data(db_plugin, handlerton *);

  if (tabledef_version.str)
    thd->lex->create_info.tabledef_version= tabledef_version;
#endif // 0

  tmp_disable_binlog(thd);

  file= mysql_create_frm_image(thd, table_s->db.str, table_s->table_name.str,
                               create_info, alter_info, C_ORDINARY_CREATE,
                               &not_used_1, &not_used_2, &frm);
  if (file)
    delete file;
  else
    rc= OPEN_FRM_CORRUPTED;

  if (!rc && frm.str) {
    table_s->option_list= 0;     // cleanup existing options ...
    table_s->option_struct= 0;   // ... if it's an assisted discovery
    rc= table_s->init_from_binary_frm_image(thd, true, frm.str, frm.length);
    } // endif frm

//ret:
  my_free(const_cast<uchar*>(frm.str));
  reenable_binlog(thd);
#if 0
  lex_end(thd->lex);
  thd->lex= old_lex;
  if (arena)
    thd->restore_active_arena(arena, &backup);
  thd->variables.sql_mode= saved_mode;
  thd->variables.character_set_client= old_cs;
#endif // 0

  if (thd->is_error() || rc) {
    thd->clear_error();
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_s->db.str,
                                       table_s->table_name.str);
    DBUG_RETURN(HA_ERR_NOT_A_TABLE);
  } else
    DBUG_RETURN(0);

} // end of init_table_share
#else   // !NEW_WAY
static int init_table_share(THD* thd,
                            TABLE_SHARE *table_s,
                            HA_CREATE_INFO *create_info,
//                          char *dsn,
                            String *sql)
{
  bool oom= false;
  PTOS topt= table_s->option_struct;

  sql->length(sql->length()-1); // remove the trailing comma
  sql->append(')');

  for (ha_create_table_option *opt= connect_table_option_list;
       opt->name; opt++) {
    ulonglong   vull;
    const char *vstr;

    switch (opt->type) {
      case HA_OPTION_TYPE_ULL:
        vull= *(ulonglong*)(((char*)topt) + opt->offset);

        if (vull != opt->def_value) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append('=');
          oom|= sql->append_ulonglong(vull);
          } // endif vull

        break;
      case HA_OPTION_TYPE_STRING:
        vstr= *(char**)(((char*)topt) + opt->offset);

        if (vstr) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append("='");
          oom|= sql->append_for_single_quote(vstr, strlen(vstr));
          oom|= sql->append('\'');
          } // endif vstr

        break;
      case HA_OPTION_TYPE_BOOL:
        vull= *(bool*)(((char*)topt) + opt->offset);

        if (vull != opt->def_value) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append('=');
          oom|= sql->append(vull ? "ON" : "OFF");
          } // endif vull

        break;
      default: // no enums here, good :)
        break;
      } // endswitch type

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endfor opt

  if (create_info->connect_string.length) {
//if (dsn) {
    oom|= sql->append(' ');
    oom|= sql->append("CONNECTION='");
    oom|= sql->append_for_single_quote(create_info->connect_string.str,
                                       create_info->connect_string.length);
//  oom|= sql->append_for_single_quote(dsn, strlen(dsn));
    oom|= sql->append('\'');

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endif string

  if (create_info->default_table_charset) {
    oom|= sql->append(' ');
    oom|= sql->append("CHARSET=");
    oom|= sql->append(create_info->default_table_charset->csname);

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endif charset

  if (xtrace)
    htrc("s_init: %.*s\n", sql->length(), sql->ptr());

  return table_s->init_from_sql_statement_string(thd, true,
                                                 sql->ptr(), sql->length());
} // end of init_table_share
#endif  // !NEW_WAY

// Add an option to the create_info option list
static void add_option(THD* thd, HA_CREATE_INFO *create_info,
                       const char *opname, const char *opval)
{
#if defined(NEW_WAY)
  LEX_STRING *opn= thd->make_lex_string(opname, strlen(opname));
  LEX_STRING *val= thd->make_lex_string(opval, strlen(opval));
  engine_option_value *pov, **start= &create_info->option_list, *end= NULL;

  for (pov= *start; pov; pov= pov->next)
    end= pov;

  pov= new(thd->mem_root) engine_option_value(*opn, *val, false, start, &end);
#endif   // NEW_WAY
} // end of add_option

// Used to check whether a MYSQL table is created on itself
bool CheckSelf(PGLOBAL g, TABLE_SHARE *s, const char *host,
                      const char *db, char *tab, const char *src, int port)
{
  if (src)
    return false;
  else if (host && stricmp(host, "localhost") && strcmp(host, "127.0.0.1"))
    return false;
  else if (db && stricmp(db, s->db.str))
    return false;
  else if (tab && stricmp(tab, s->table_name.str))
    return false;
  else if (port && port != (signed)GetDefaultPort())
    return false;

  strcpy(g->Message, "This MySQL table is defined on itself");
  return true;
} // end of CheckSelf

/**
  @brief
  connect_assisted_discovery() is called when creating a table with no columns.

  @details
  When assisted discovery is used the .frm file have not already been
  created. You can overwrite some definitions at this point but the
  main purpose of it is to define the columns for some table types.

  @note
  this function is no more called in case of CREATE .. SELECT
*/
static int connect_assisted_discovery(handlerton *hton, THD* thd,
                                      TABLE_SHARE *table_s,
                                      HA_CREATE_INFO *create_info)
{
  char        v=0, spc= ',', qch= 0;
  const char *fncn= "?";
  const char *user, *fn, *db, *host, *pwd, *sep, *tbl, *src;
  const char *col, *ocl, *rnk, *pic, *fcl, *skc;
  char       *tab, *dsn, *shm, *dpath; 
#if defined(WIN32)
  char       *nsp= NULL, *cls= NULL;
#endif   // WIN32
  int         port= 0, hdr= 0, mxr __attribute__((unused))= 0, mxe= 0, rc= 0;
  int         cop __attribute__((unused)) = 0;
  uint        tm, fnc= FNC_NO, supfnc= (FNC_NO | FNC_COL);
  bool        bif, ok= false, dbf= false;
  TABTYPE     ttp= TAB_UNDEF;
  PQRYRES     qrp= NULL;
  PCOLRES     crp;
  PCONNECT    xp= NULL;
  PGLOBAL     g= GetPlug(thd, xp);
  PDBUSER     dup= PlgGetUser(g);
  PCATLG      cat= (dup) ? dup->Catalog : NULL;
  PTOS        topt= table_s->option_struct;
#if defined(NEW_WAY)
//CHARSET_INFO *cs;
  Alter_info  alter_info;
#else   // !NEW_WAY
  char        buf[1024];
  String      sql(buf, sizeof(buf), system_charset_info);

  sql.copy(STRING_WITH_LEN("CREATE TABLE whatever ("), system_charset_info);
#endif  // !NEW_WAY

  if (!g)
    return HA_ERR_INTERNAL_ERROR;

  user= host= pwd= tbl= src= col= ocl= pic= fcl= skc= rnk= dsn= NULL;

  // Get the useful create options
  ttp= GetTypeID(topt->type);
  fn=  topt->filename;
  tab= (char*)topt->tabname;
  src= topt->srcdef;
  db=  topt->dbname;
  fncn= topt->catfunc;
  fnc= GetFuncID(fncn);
  sep= topt->separator;
  spc= (!sep || !strcmp(sep, "\\t")) ? '\t' : *sep;
  qch= topt->qchar ? *topt->qchar : (signed)topt->quoted >= 0 ? '"' : 0;
  hdr= (int)topt->header;
  tbl= topt->tablist;
  col= topt->colist;

  if (topt->oplist) {
    host= GetListOption(g, "host", topt->oplist, "localhost");
    user= GetListOption(g, "user", topt->oplist, "root");
    // Default value db can come from the DBNAME=xxx option.
    db= GetListOption(g, "database", topt->oplist, db);
    col= GetListOption(g, "colist", topt->oplist, col);
    ocl= GetListOption(g, "occurcol", topt->oplist, NULL);
    pic= GetListOption(g, "pivotcol", topt->oplist, NULL);
    fcl= GetListOption(g, "fnccol", topt->oplist, NULL);
    skc= GetListOption(g, "skipcol", topt->oplist, NULL);
    rnk= GetListOption(g, "rankcol", topt->oplist, NULL);
    pwd= GetListOption(g, "password", topt->oplist);
#if defined(WIN32)
    nsp= GetListOption(g, "namespace", topt->oplist);
    cls= GetListOption(g, "class", topt->oplist);
#endif   // WIN32
    port= atoi(GetListOption(g, "port", topt->oplist, "0"));
#if defined(ODBC_SUPPORT)
    mxr= atoi(GetListOption(g,"maxres", topt->oplist, "0"));
#endif
    mxe= atoi(GetListOption(g,"maxerr", topt->oplist, "0"));
#if defined(PROMPT_OK)
    cop= atoi(GetListOption(g, "checkdsn", topt->oplist, "0"));
#endif   // PROMPT_OK
  } else {
    host= "localhost";
    user= "root";
  } // endif option_list

  if (!(shm= (char*)db))
    db= table_s->db.str;                     // Default value

  // Check table type
  if (ttp == TAB_UNDEF) {
    topt->type= (src) ? "MYSQL" : (tab) ? "PROXY" : "DOS";
    ttp= GetTypeID(topt->type);
    sprintf(g->Message, "No table_type. Was set to %s", topt->type);
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
    add_option(thd, create_info, "table_type", topt->type);
  } else if (ttp == TAB_NIY) {
    sprintf(g->Message, "Unsupported table type %s", topt->type);
    my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
    return HA_ERR_INTERNAL_ERROR;
  } // endif ttp

  if (!tab) {
    if (ttp == TAB_TBL) {
      // Make tab the first table of the list
      char *p;

      if (!tbl) {
        strcpy(g->Message, "Missing table list");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        return HA_ERR_INTERNAL_ERROR;
        } // endif tbl

      tab= (char*)PlugSubAlloc(g, NULL, strlen(tbl) + 1);
      strcpy(tab, tbl);

      if ((p= strchr(tab, ',')))
        *p= 0;

      if ((p=strchr(tab, '.'))) {
        *p= 0;
        db= tab;
        tab= p + 1;
        } // endif p

    } else if (ttp != TAB_ODBC || !(fnc & (FNC_TABLE | FNC_COL)))
      tab= table_s->table_name.str;              // Default value

#if defined(NEW_WAY)
//  add_option(thd, create_info, "tabname", tab);
#endif   // NEW_WAY
    } // endif tab

  switch (ttp) {
#if defined(ODBC_SUPPORT)
    case TAB_ODBC:
      dsn= create_info->connect_string.str;

      if (fnc & (FNC_DSN | FNC_DRIVER)) {
        ok= true;
#if defined(PROMPT_OK)
      } else if (!stricmp(thd->main_security_ctx.host, "localhost")
                && cop == 1) {
        if ((dsn = ODBCCheckConnection(g, dsn, cop)) != NULL) {
          thd->make_lex_string(&create_info->connect_string, dsn, strlen(dsn));
          ok= true;
          } // endif dsn
#endif   // PROMPT_OK

      } else if (!dsn)
        sprintf(g->Message, "Missing %s connection string", topt->type);
      else
        ok= true;

      supfnc |= (FNC_TABLE | FNC_DSN | FNC_DRIVER);
      break;
#endif   // ODBC_SUPPORT
    case TAB_DBF:
      dbf= true;
      // Passthru
    case TAB_CSV:
      if (!fn && fnc != FNC_NO)
        sprintf(g->Message, "Missing %s file name", topt->type);
      else
        ok= true;

      break;
#if defined(MYSQL_SUPPORT)
    case TAB_MYSQL:
      ok= true;

      if (create_info->connect_string.str) {
        int     len= create_info->connect_string.length;
        PMYDEF  mydef= new(g) MYSQLDEF();

        dsn= (char*)PlugSubAlloc(g, NULL, len + 1);
        strncpy(dsn, create_info->connect_string.str, len);
        dsn[len]= 0;
        mydef->SetName(create_info->alias);

        if (!mydef->ParseURL(g, dsn, false)) {
          if (mydef->GetHostname())
            host= mydef->GetHostname();

          if (mydef->GetUsername())
            user= mydef->GetUsername();

          if (mydef->GetPassword())
            pwd=  mydef->GetPassword();

          if (mydef->GetDatabase())
            db= mydef->GetDatabase();

          if (mydef->GetTabname())
            tab= mydef->GetTabname();

          if (mydef->GetPortnumber())
            port= mydef->GetPortnumber();

        } else
          ok= false;

      } else if (!user)
        user= "root";

      if (ok && CheckSelf(g, table_s, host, db, tab, src, port))
        ok= false;

      break;
#endif   // MYSQL_SUPPORT
#if defined(WIN32)
    case TAB_WMI:
      ok= true;
      break;
#endif   // WIN32
    case TAB_PIVOT:
      supfnc= FNC_NO;
    case TAB_PRX:
    case TAB_TBL:
    case TAB_XCL:
    case TAB_OCCUR:
      if (!src && !stricmp(tab, create_info->alias) &&
         (!db || !stricmp(db, table_s->db.str)))
        sprintf(g->Message, "A %s table cannot refer to itself", topt->type);
      else
        ok= true;

      break;
    case TAB_OEM:
      if (topt->module && topt->subtype)
        ok= true;
      else
        strcpy(g->Message, "Missing OEM module or subtype");

      break;
    default:
      sprintf(g->Message, "Cannot get column info for table type %s", topt->type);
      break;
    } // endif ttp

  // Check for supported catalog function
  if (ok && !(supfnc & fnc)) {
    sprintf(g->Message, "Unsupported catalog function %s for table type %s",
                        fncn, topt->type);
    ok= false;
    } // endif supfnc

  if (src && fnc != FNC_NO) {
    strcpy(g->Message, "Cannot make catalog table from srcdef");
    ok= false;
    } // endif src

  if (ok) {
    char   *cnm, *rem, *dft, *xtra;
    int     i, len, prec, dec, typ, flg;

//  if (cat)
//    cat->SetDataPath(g, table_s->db.str);
//  else
//    return HA_ERR_INTERNAL_ERROR;           // Should never happen

    dpath= SetPath(g, table_s->db.str);

    if (src && ttp != TAB_PIVOT && ttp != TAB_ODBC) {
      qrp= SrcColumns(g, host, db, user, pwd, src, port);

      if (qrp && ttp == TAB_OCCUR)
        if (OcrSrcCols(g, qrp, col, ocl, rnk)) {
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          return HA_ERR_INTERNAL_ERROR;
          } // endif OcrSrcCols

    } else switch (ttp) {
      case TAB_DBF:
        qrp= DBFColumns(g, dpath, fn, fnc == FNC_COL);
        break;
#if defined(ODBC_SUPPORT)
      case TAB_ODBC:
        switch (fnc) {
          case FNC_NO:
          case FNC_COL:
            if (src) {
              qrp= ODBCSrcCols(g, dsn, (char*)src);
              src= NULL;     // for next tests
            } else
              qrp= ODBCColumns(g, dsn, shm, tab, NULL, mxr, fnc == FNC_COL);

            break;
          case FNC_TABLE:
            qrp= ODBCTables(g, dsn, shm, tab, mxr, true);
            break;
          case FNC_DSN:
            qrp= ODBCDataSources(g, mxr, true);
            break;
          case FNC_DRIVER:
            qrp= ODBCDrivers(g, mxr, true);
            break;
          default:
            sprintf(g->Message, "invalid catfunc %s", fncn);
            break;
        } // endswitch info

        break;
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
      case TAB_MYSQL:
        qrp= MyColumns(g, thd, host, db, user, pwd, tab,
                       NULL, port, fnc == FNC_COL);
        break;
#endif   // MYSQL_SUPPORT
      case TAB_CSV:
        qrp= CSVColumns(g, dpath, fn, spc, qch, hdr, mxe, fnc == FNC_COL);
        break;
#if defined(WIN32)
      case TAB_WMI:
        qrp= WMIColumns(g, nsp, cls, fnc == FNC_COL);
        break;
#endif   // WIN32
      case TAB_PRX:
      case TAB_TBL:
      case TAB_XCL:
      case TAB_OCCUR:
        bif= fnc == FNC_COL;
        qrp= TabColumns(g, thd, db, tab, bif);

        if (!qrp && bif && fnc != FNC_COL)         // tab is a view
          qrp= MyColumns(g, thd, host, db, user, pwd, tab, NULL, port, false);

        if (qrp && ttp == TAB_OCCUR && fnc != FNC_COL)
          if (OcrColumns(g, qrp, col, ocl, rnk)) {
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            return HA_ERR_INTERNAL_ERROR;
            } // endif OcrColumns

        break;
      case TAB_PIVOT:
        qrp= PivotColumns(g, tab, src, pic, fcl, skc, host, db, user, pwd, port);
        break;
      case TAB_OEM:
        qrp= OEMColumns(g, topt, tab, (char*)db, fnc == FNC_COL);
        break;
      default:
        strcpy(g->Message, "System error during assisted discovery");
        break;
      } // endswitch ttp

    if (!qrp) {
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      return HA_ERR_INTERNAL_ERROR;
      } // endif !qrp

    if (fnc != FNC_NO || src || ttp == TAB_PIVOT) {
      // Catalog like table
      for (crp= qrp->Colresp; !rc && crp; crp= crp->Next) {
        cnm= encode(g, crp->Name);
        typ= crp->Type;
        len= crp->Length;
        dec= crp->Prec;
        flg= crp->Flag;
        v= crp->Var;

        if (!len && typ == TYPE_STRING)
          len= 256;      // STRBLK's have 0 length

        // Now add the field
#if defined(NEW_WAY)
        rc= add_fields(g, thd, &alter_info, cnm, typ, len, dec,
                       NOT_NULL_FLAG, "", flg, dbf, v);
#else   // !NEW_WAY
        if (add_field(&sql, cnm, typ, len, dec, NOT_NULL_FLAG,
                      NULL, NULL, NULL, flg, dbf, v))
          rc= HA_ERR_OUT_OF_MEM;
#endif  // !NEW_WAY
      } // endfor crp

    } else {            
      // Not a catalog table
      if (!qrp->Nblin) {
        if (tab)
          sprintf(g->Message, "Cannot get columns from %s", tab);
        else
          strcpy(g->Message, "Fail to retrieve columns");

        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        return HA_ERR_INTERNAL_ERROR;
        } // endif !nblin

      for (i= 0; !rc && i < qrp->Nblin; i++) {
        typ= len= prec= dec= 0;
        tm= NOT_NULL_FLAG;
        cnm= (char*)"noname";
        dft= xtra= NULL;
#if defined(NEW_WAY)
        rem= "";
//      cs= NULL;
#else   // !NEW_WAY
        rem= NULL;
#endif  // !NEW_WAY

        for (crp= qrp->Colresp; crp; crp= crp->Next)
          switch (crp->Fld) {
            case FLD_NAME:
              cnm= encode(g, crp->Kdata->GetCharValue(i));
              break;
            case FLD_TYPE:
              typ= crp->Kdata->GetIntValue(i);
              v = (crp->Nulls) ? crp->Nulls[i] : 0;
              break;
            case FLD_PREC:
              // PREC must be always before LENGTH
              len= prec= crp->Kdata->GetIntValue(i);
              break;
            case FLD_LENGTH:
              len= crp->Kdata->GetIntValue(i);
              break;
            case FLD_SCALE:
              dec= crp->Kdata->GetIntValue(i);
              break;
            case FLD_NULL:
              if (crp->Kdata->GetIntValue(i))
                tm= 0;               // Nullable

              break;
            case FLD_REM:
              rem= crp->Kdata->GetCharValue(i);
              break;
//          case FLD_CHARSET:
              // No good because remote table is already translated
//            if (*(csn= crp->Kdata->GetCharValue(i)))
//              cs= get_charset_by_name(csn, 0);

//            break;
            case FLD_DEFAULT:
              dft= crp->Kdata->GetCharValue(i);
              break;
            case FLD_EXTRA:
              xtra= crp->Kdata->GetCharValue(i);

              // Auto_increment is not supported yet
              if (!stricmp(xtra, "AUTO_INCREMENT"))
                xtra= NULL;

              break;
            default:
              break;                 // Ignore
            } // endswitch Fld

#if defined(ODBC_SUPPORT)
        if (ttp == TAB_ODBC) {
          int plgtyp;

          // typ must be PLG type, not SQL type
          if (!(plgtyp= TranslateSQLType(typ, dec, prec, v))) {
            sprintf(g->Message, "Unsupported SQL type %d", typ);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            return HA_ERR_INTERNAL_ERROR;
          } else
            typ= plgtyp;

          switch (typ) {
            case TYPE_DOUBLE:
              // Some data sources do not count dec in length (prec)
              prec += (dec + 2);        // To be safe
            case TYPE_DECIM:
              break;
            default:
              dec= 0;
            } // endswitch typ

          } // endif ttp
#endif   // ODBC_SUPPORT

        // Make the arguments as required by add_fields
        if (typ == TYPE_DATE)
          prec= 0;
        else if (typ == TYPE_DOUBLE)
          prec= len;

        // Now add the field
#if defined(NEW_WAY)
        rc= add_fields(g, thd, &alter_info, cnm, typ, prec, dec,
                       tm, rem, 0, dbf, v);
#else   // !NEW_WAY
        if (add_field(&sql, cnm, typ, prec, dec, tm, rem, dft, xtra,
                      0, dbf, v))
          rc= HA_ERR_OUT_OF_MEM;
#endif  // !NEW_WAY
        } // endfor i

    } // endif fnc

#if defined(NEW_WAY)
    rc= init_table_share(thd, table_s, create_info, &alter_info);
#else   // !NEW_WAY
    if (!rc)
      rc= init_table_share(thd, table_s, create_info, &sql);
//    rc= init_table_share(thd, table_s, create_info, dsn, &sql);
#endif   // !NEW_WAY

    return rc;
    } // endif ok

  my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
  return HA_ERR_INTERNAL_ERROR;
} // end of connect_assisted_discovery

/**
  Get the database name from a qualified table name.
*/
char *ha_connect::GetDBfromName(const char *name)
{
  char *db, dbname[128], tbname[128];

  if (filename_to_dbname_and_tablename(name, dbname, sizeof(dbname),
                                             tbname, sizeof(tbname)))
    *dbname= 0;

  if (*dbname) {
    assert(xp && xp->g);
    db= (char*)PlugSubAlloc(xp->g, NULL, strlen(dbname + 1));
    strcpy(db, dbname);
  } else
    db= NULL;

  return db;
} // end of GetDBfromName


/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @note
  Currently we do some checking on the create definitions and stop
  creating if an error is found. We wish we could change the table
  definition such as providing a default table type. However, as said
  above, there are no method to do so.

  @see
  ha_create_table() in handle.cc
*/

int ha_connect::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  int     rc= RC_OK;
  bool    dbf, inward;
  Field* *field;
  Field  *fp;
  TABTYPE type;
  TABLE  *st= table;                       // Probably unuseful
  THD    *thd= ha_thd();
#if defined(WITH_PARTITION_STORAGE_ENGINE)
  partition_info *part_info= table_arg->part_info;
#endif   // WITH_PARTITION_STORAGE_ENGINE
  xp= GetUser(thd, xp);
  PGLOBAL g= xp->g;

  DBUG_ENTER("ha_connect::create");
  int  sqlcom= thd_sql_command(table_arg->in_use);
  PTOS options= GetTableOptionStruct(table_arg->s);

  table= table_arg;         // Used by called functions

  if (xtrace)
    htrc("create: this=%p thd=%p xp=%p g=%p sqlcom=%d name=%s\n",
           this, thd, xp, g, sqlcom, GetTableName());

  // CONNECT engine specific table options:
  DBUG_ASSERT(options);
  type= GetTypeID(options->type);

  // Check table type
  if (type == TAB_UNDEF) {
    options->type= (options->srcdef)  ? "MYSQL" :
                   (options->tabname) ? "PROXY" : "DOS";
    type= GetTypeID(options->type);
    sprintf(g->Message, "No table_type. Will be set to %s", options->type);

    if (sqlcom == SQLCOM_CREATE_TABLE)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);

  } else if (type == TAB_NIY) {
    sprintf(g->Message, "Unsupported table type %s", options->type);
    my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  } // endif ttp

  if (check_privileges(thd, options, GetDBfromName(name)))
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  inward= IsFileType(type) && !options->filename;

  if (options->data_charset) {
    const CHARSET_INFO *data_charset;

    if (!(data_charset= get_charset_by_csname(options->data_charset,
                                              MY_CS_PRIMARY, MYF(0)))) {
      my_error(ER_UNKNOWN_CHARACTER_SET, MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endif charset

    if (type == TAB_XML && data_charset != &my_charset_utf8_general_ci) {
      my_printf_error(ER_UNKNOWN_ERROR,
                      "DATA_CHARSET='%s' is not supported for TABLE_TYPE=XML",
                        MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endif utf8

    } // endif charset

  if (!g) {
    rc= HA_ERR_INTERNAL_ERROR;
    DBUG_RETURN(rc);
  } else
    dbf= (GetTypeID(options->type) == TAB_DBF && !options->catfunc);

  // Can be null in ALTER TABLE
  if (create_info->alias)
    // Check whether a table is defined on itself
    switch (type) {
      case TAB_PRX:
      case TAB_XCL:
      case TAB_PIVOT:
      case TAB_OCCUR:
        if (options->srcdef) {
          strcpy(g->Message, "Cannot check looping reference");
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        } else if (options->tabname) {
          if (!stricmp(options->tabname, create_info->alias) &&
             (!options->dbname || !stricmp(options->dbname, table_arg->s->db.str))) {
            sprintf(g->Message, "A %s table cannot refer to itself",
                                options->type);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
            } // endif tab

        } else {
          strcpy(g->Message, "Missing object table name or definition");
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
        } // endif tabname

      case TAB_MYSQL:
#if defined(WITH_PARTITION_STORAGE_ENGINE)
        if (!part_info)
#endif   // WITH_PARTITION_STORAGE_ENGINE
       {const char *src= options->srcdef;
        char *host, *db, *tab= (char*)options->tabname;
        int   port;

        host= GetListOption(g, "host", options->oplist, NULL);
        db= GetStringOption("database", NULL);
        port= atoi(GetListOption(g, "port", options->oplist, "0"));

        if (create_info->connect_string.str) {
          char   *dsn;
          int     len= create_info->connect_string.length;
          PMYDEF  mydef= new(g) MYSQLDEF();

          dsn= (char*)PlugSubAlloc(g, NULL, len + 1);
          strncpy(dsn, create_info->connect_string.str, len);
          dsn[len]= 0;
          mydef->SetName(create_info->alias);

          if (!mydef->ParseURL(g, dsn, false)) {
            if (mydef->GetHostname())
              host= mydef->GetHostname();

            if (mydef->GetDatabase())
              db= mydef->GetDatabase();

            if (mydef->GetTabname())
              tab= mydef->GetTabname();

            if (mydef->GetPortnumber())
              port= mydef->GetPortnumber();

          } else {
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
          } // endif ParseURL

          } // endif connect_string

        if (CheckSelf(g, table_arg->s, host, db, tab, src, port)) {
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
          } // endif CheckSelf

       }break;
      default: /* do nothing */;
        break;
     } // endswitch ttp

  if (type == TAB_XML) {
    bool  dom;                  // True: MS-DOM, False libxml2
    char *xsup= GetListOption(g, "Xmlsup", options->oplist, "*");

    // Note that if no support is specified, the default is MS-DOM
    // on Windows and libxml2 otherwise
    switch (*xsup) {
      case '*':
#if defined(WIN32)
        dom= true;
#else   // !WIN32
        dom= false;
#endif  // !WIN32
        break;
      case 'M':
      case 'D':
        dom= true;
        break;
      default:
        dom= false;
        break;
      } // endswitch xsup

#if !defined(DOMDOC_SUPPORT)
    if (dom) {
      strcpy(g->Message, "MS-DOM not supported by this version");
      xsup= NULL;
      } // endif DomDoc
#endif   // !DOMDOC_SUPPORT

#if !defined(LIBXML2_SUPPORT)
    if (!dom) {
      strcpy(g->Message, "libxml2 not supported by this version");
      xsup= NULL;
      } // endif Libxml2
#endif   // !LIBXML2_SUPPORT

    if (!xsup) {
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif xsup

    } // endif type

  // Check column types
  for (field= table_arg->field; *field; field++) {
    fp= *field;

    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column

    if (fp->flags & AUTO_INCREMENT_FLAG) {
      strcpy(g->Message, "Auto_increment is not supported yet");
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif flags

    if (fp->flags & (BLOB_FLAG | ENUM_FLAG | SET_FLAG)) {
      sprintf(g->Message, "Unsupported type for column %s",
                          fp->field_name);
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif flags

    switch (fp->type()) {
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_INT24:
        break;                     // Ok
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
        if (!fp->field_length) {
          sprintf(g->Message, "Unsupported 0 length for column %s",
                              fp->field_name);
          rc= HA_ERR_INTERNAL_ERROR;
          my_printf_error(ER_UNKNOWN_ERROR,
                          "Unsupported 0 length for column %s",
                          MYF(0), fp->field_name);
          DBUG_RETURN(rc);
          } // endif fp

        break;                     // To be checked
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      default:
//      fprintf(stderr, "Unsupported type column %s\n", fp->field_name);
        sprintf(g->Message, "Unsupported type for column %s",
                            fp->field_name);
        rc= HA_ERR_INTERNAL_ERROR;
        my_printf_error(ER_UNKNOWN_ERROR, "Unsupported type for column %s",
                        MYF(0), fp->field_name);
        DBUG_RETURN(rc);
        break;
      } // endswitch type

    if ((fp)->real_maybe_null() && !IsTypeNullable(type)) {
      my_printf_error(ER_UNKNOWN_ERROR,
                      "Table type %s does not support nullable columns",
                      MYF(0), options->type);
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
      } // endif !nullable

    if (dbf) {
      bool b= false;

      if ((b= strlen(fp->field_name) > 10))
        sprintf(g->Message, "DBF: Column name '%s' is too long (max=10)",
                            fp->field_name);
      else if ((b= fp->field_length > 255))
        sprintf(g->Message, "DBF: Column length too big for '%s' (max=255)",
                            fp->field_name);

      if (b) {
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_INTERNAL_ERROR;
        DBUG_RETURN(rc);
        } // endif b

      } // endif dbf

    } // endfor field

  if ((sqlcom == SQLCOM_CREATE_TABLE || *GetTableName() == '#') && inward) {
    // The file name is not specified, create a default file in
    // the database directory named table_name.table_type.
    // (temporarily not done for XML because a void file causes
    // the XML parsers to report an error on the first Insert)
    char buf[256], fn[_MAX_PATH], dbpath[128], lwt[12];
    int  h;

    // Check for incompatible options
    if (options->sepindex) {
      my_message(ER_UNKNOWN_ERROR,
            "SEPINDEX is incompatible with unspecified file name",
            MYF(0));
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
    } else if (GetTypeID(options->type) == TAB_VEC)
      if (!table->s->max_rows || options->split) {
        my_printf_error(ER_UNKNOWN_ERROR,
            "%s tables whose file name is unspecified cannot be split",
            MYF(0), options->type);
        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      } else if (options->header == 2) {
        my_printf_error(ER_UNKNOWN_ERROR,
        "header=2 is not allowed for %s tables whose file name is unspecified",
            MYF(0), options->type);
        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      } // endif's

    // Fold type to lower case
    for (int i= 0; i < 12; i++)
      if (!options->type[i]) {
        lwt[i]= 0;
        break;
      } else
        lwt[i]= tolower(options->type[i]);

#if defined(WITH_PARTITION_STORAGE_ENGINE)
    if (part_info) {
      char *p;

      strcpy(dbpath, name);
      p= strrchr(dbpath, slash);
      strcpy(partname, ++p);
      strcat(strcat(strcpy(buf, p), "."), lwt);
      *p= 0;
    } else {
#endif   // WITH_PARTITION_STORAGE_ENGINE
      strcat(strcat(strcpy(buf, GetTableName()), "."), lwt);
      sprintf(g->Message, "No file name. Table will use %s", buf);
  
      if (sqlcom == SQLCOM_CREATE_TABLE)
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
  
      strcat(strcat(strcpy(dbpath, "./"), table->s->db.str), "/");
#if defined(WITH_PARTITION_STORAGE_ENGINE)
    } // endif part_info
#endif   // WITH_PARTITION_STORAGE_ENGINE

    PlugSetPath(fn, buf, dbpath);

    if ((h= ::open(fn, O_CREAT | O_EXCL, 0666)) == -1) {
      if (errno == EEXIST)
        sprintf(g->Message, "Default file %s already exists", fn);
      else
        sprintf(g->Message, "Error %d creating file %s", errno, fn);

      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
    } else
      ::close(h);
    
    if ((type == TAB_FMT || options->readonly) && sqlcom == SQLCOM_CREATE_TABLE)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0,
        "Congratulation, you just created a read-only void table!");

    } // endif sqlcom

  if (xtrace)
    htrc("xchk=%p createas=%d\n", g->Xchk, g->Createas);

  // To check whether indexes have to be made or remade
  if (!g->Xchk) {
    PIXDEF xdp;

    // We should be in CREATE TABLE, ALTER_TABLE or CREATE INDEX
    if (!(sqlcom == SQLCOM_CREATE_TABLE || sqlcom == SQLCOM_ALTER_TABLE ||
          sqlcom == SQLCOM_CREATE_INDEX || sqlcom == SQLCOM_DROP_INDEX))  
//         (sqlcom == SQLCOM_CREATE_INDEX && part_info) ||  
//         (sqlcom == SQLCOM_DROP_INDEX && part_info)))  
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0,
        "Unexpected command in create, please contact CONNECT team");

#if defined(WITH_PARTITION_STORAGE_ENGINE)
    if (part_info && !inward)
      strcpy(partname, decode(g, strrchr(name, '#') + 1));
//    strcpy(partname, part_info->curr_part_elem->partition_name);
#endif   // WITH_PARTITION_STORAGE_ENGINE

    if (g->Alchecked == 0 &&
        (!IsFileType(type) || FileExists(options->filename, false))) {
      if (part_info) {
        sprintf(g->Message, "Data repartition in %s is unchecked", partname); 
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
      } else if (sqlcom == SQLCOM_ALTER_TABLE) {
        // This is an ALTER to CONNECT from another engine.
        // It cannot be accepted because the table data would be modified
        // except when the target file does not exist.
        strcpy(g->Message, "Operation denied. Table data would be modified.");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endif part_info

      } // endif outward

    // Get the index definitions
    if ((xdp= GetIndexInfo()) || sqlcom == SQLCOM_DROP_INDEX) {
      if (options->multiple) {
        strcpy(g->Message, "Multiple tables are not indexable");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_UNSUPPORTED;
      } else if (options->compressed) {
        strcpy(g->Message, "Compressed tables are not indexable");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_UNSUPPORTED;
      } else if (GetIndexType(type) == 1) {
        PDBUSER dup= PlgGetUser(g);
        PCATLG  cat= (dup) ? dup->Catalog : NULL;

        SetDataPath(g, table_arg->s->db.str);

        if (cat) {
//        cat->SetDataPath(g, table_arg->s->db.str);

#if defined(WITH_PARTITION_STORAGE_ENGINE)
          if (part_info)
            strcpy(partname, 
                   decode(g, strrchr(name, (inward ? slash : '#')) + 1));
#endif   // WITH_PARTITION_STORAGE_ENGINE

          if ((rc= optimize(table->in_use, NULL))) {
            htrc("Create rc=%d %s\n", rc, g->Message);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            rc= HA_ERR_INTERNAL_ERROR;
          } else
            CloseTable(g);

          } // endif cat
    
      } else if (!GetIndexType(type)) {
        sprintf(g->Message, "Table type %s is not indexable", options->type);
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_UNSUPPORTED;
      } // endif index type

      } // endif xdp

  } else {
    // This should not happen anymore with indexing new way
    my_message(ER_UNKNOWN_ERROR,
               "CONNECT index modification should be in-place", MYF(0));
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  } // endif Xchk

  table= st;
  DBUG_RETURN(rc);
} // end of create

/**
  Used to check whether a file based outward table can be populated by
  an ALTER TABLE command. The conditions are:
  - file does not exist or is void
  - user has file privilege
*/
bool ha_connect::FileExists(const char *fn, bool bf)
{
  if (!fn || !*fn)
    return false;
  else if (IsPartitioned() && bf)
    return true;

  if (table) {
    char *s, tfn[_MAX_PATH], filename[_MAX_PATH], path[128];
    bool  b= false;
    int   n;
    struct stat info;

    if (check_access(ha_thd(), FILE_ACL, table->s->db.str,
                     NULL, NULL, 0, 0))
      return true;

#if defined(WIN32)
    s= "\\";
#else   // !WIN32
    s= "/";
#endif  // !WIN32
    if (IsPartitioned()) {
      sprintf(tfn, fn, GetPartName());

      // This is to avoid an initialization error raised by the
      // test on check_table_flags made in ha_partition::open
      // that can fail if some partition files are empty.
      b= true;
    } else
      strcpy(tfn, fn);

    strcat(strcat(strcat(strcpy(path, "."), s), table->s->db.str), s);
    PlugSetPath(filename, tfn, path);
    n= stat(filename, &info);

    if (n < 0) {
      if (errno != ENOENT) {
        char buf[_MAX_PATH + 20];

        sprintf(buf, "Error %d for file %s", errno, filename);
        push_warning(table->in_use, Sql_condition::WARN_LEVEL_WARN, 0, buf);
        return true;
      } else
        return false;

    } else
      return (info.st_size || b) ? true : false;

    } // endif table

  return true;
} // end of FileExists

// Called by SameString and NoFieldOptionChange
bool ha_connect::CheckString(const char *str1, const char *str2)
{
  bool  b1= (!str1 || !*str1), b2= (!str2 || !*str2);

  if (b1 && b2)
    return true;
  else if ((b1 && !b2) || (!b1 && b2) || stricmp(str1, str2))
    return false;

  return true;
} // end of CheckString

/**
  check whether a string option have changed
  */
bool ha_connect::SameString(TABLE *tab, char *opn)
{
  char *str1, *str2;

  tshp= tab->s;                 // The altered table
  str1= GetStringOption(opn);
  tshp= NULL;
  str2= GetStringOption(opn);
  return CheckString(str1, str2);
} // end of SameString

/**
  check whether a Boolean option have changed
  */
bool ha_connect::SameBool(TABLE *tab, char *opn)
{
  bool b1, b2;

  tshp= tab->s;                 // The altered table
  b1= GetBooleanOption(opn, false);
  tshp= NULL;
  b2= GetBooleanOption(opn, false);
  return (b1 == b2);
} // end of SameBool

/**
  check whether an integer option have changed
  */
bool ha_connect::SameInt(TABLE *tab, char *opn)
{
  int i1, i2;

  tshp= tab->s;                 // The altered table
  i1= GetIntegerOption(opn);
  tshp= NULL;
  i2= GetIntegerOption(opn);

  if (!stricmp(opn, "lrecl"))
    return (i1 == i2 || !i1 || !i2);
  else if (!stricmp(opn, "ending"))
    return (i1 == i2 || i1 <= 0 || i2 <= 0);
  else
    return (i1 == i2);

} // end of SameInt

/**
  check whether a field option have changed
  */
bool ha_connect::NoFieldOptionChange(TABLE *tab)
{
  bool rc= true;
  ha_field_option_struct *fop1, *fop2;
  Field* *fld1= table->s->field;
  Field* *fld2= tab->s->field;

  for (; rc && *fld1 && *fld2; fld1++, fld2++) {
    fop1= (*fld1)->option_struct;
    fop2= (*fld2)->option_struct;

    rc= (fop1->offset == fop2->offset &&
         fop1->fldlen == fop2->fldlen &&
         CheckString(fop1->dateformat, fop2->dateformat) &&
         CheckString(fop1->fieldformat, fop2->fieldformat) &&
         CheckString(fop1->special, fop2->special));
    } // endfor fld

  return rc;
} // end of NoFieldOptionChange

 /**
    Check if a storage engine supports a particular alter table in-place

    @param    altered_table     TABLE object for new version of table.
    @param    ha_alter_info     Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.

    @retval   HA_ALTER_ERROR                  Unexpected error.
    @retval   HA_ALTER_INPLACE_NOT_SUPPORTED  Not supported, must use copy.
    @retval   HA_ALTER_INPLACE_EXCLUSIVE_LOCK Supported, but requires X lock.
    @retval   HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE
                                              Supported, but requires SNW lock
                                              during main phase. Prepare phase
                                              requires X lock.
    @retval   HA_ALTER_INPLACE_SHARED_LOCK    Supported, but requires SNW lock.
    @retval   HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE
                                              Supported, concurrent reads/writes
                                              allowed. However, prepare phase
                                              requires X lock.
    @retval   HA_ALTER_INPLACE_NO_LOCK        Supported, concurrent
                                              reads/writes allowed.

    @note The default implementation uses the old in-place ALTER API
    to determine if the storage engine supports in-place ALTER or not.

    @note Called without holding thr_lock.c lock.
 */
enum_alter_inplace_result
ha_connect::check_if_supported_inplace_alter(TABLE *altered_table,
                                Alter_inplace_info *ha_alter_info)
{
  DBUG_ENTER("check_if_supported_alter");

  bool            idx= false, outward= false;
  THD            *thd= ha_thd();
  int             sqlcom= thd_sql_command(thd);
  TABTYPE         newtyp, type= TAB_UNDEF;
  HA_CREATE_INFO *create_info= ha_alter_info->create_info;
  PTOS            newopt, oldopt;
  xp= GetUser(thd, xp);
  PGLOBAL         g= xp->g;

  if (!g || !table) {
    my_message(ER_UNKNOWN_ERROR, "Cannot check ALTER operations", MYF(0));
    DBUG_RETURN(HA_ALTER_ERROR);
    } // endif Xchk

  newopt= altered_table->s->option_struct;
  oldopt= table->s->option_struct;

  // If this is the start of a new query, cleanup the previous one
  if (xp->CheckCleanup()) {
    tdbp= NULL;
    valid_info= false;
    } // endif CheckCleanup

  g->Alchecked= 1;       // Tested in create
  g->Xchk= NULL;
  type= GetRealType(oldopt);
  newtyp= GetRealType(newopt);

  // No copy algorithm for outward tables
  outward= (!IsFileType(type) || (oldopt->filename && *oldopt->filename));

  // Index operations
  Alter_inplace_info::HA_ALTER_FLAGS index_operations=
    Alter_inplace_info::ADD_INDEX |
    Alter_inplace_info::DROP_INDEX |
    Alter_inplace_info::ADD_UNIQUE_INDEX |
    Alter_inplace_info::DROP_UNIQUE_INDEX |
    Alter_inplace_info::ADD_PK_INDEX |
    Alter_inplace_info::DROP_PK_INDEX;

  Alter_inplace_info::HA_ALTER_FLAGS inplace_offline_operations=
    Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH |
    Alter_inplace_info::ALTER_COLUMN_NAME |
    Alter_inplace_info::ALTER_COLUMN_DEFAULT |
    Alter_inplace_info::CHANGE_CREATE_OPTION |
    Alter_inplace_info::ALTER_RENAME |
    Alter_inplace_info::ALTER_PARTITIONED | index_operations;

  if (ha_alter_info->handler_flags & index_operations ||
      !SameString(altered_table, "optname") ||
      !SameBool(altered_table, "sepindex")) {
    if (newopt->multiple) {
      strcpy(g->Message, "Multiple tables are not indexable");
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      DBUG_RETURN(HA_ALTER_ERROR);
    } else if (newopt->compressed) {
      strcpy(g->Message, "Compressed tables are not indexable");
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      DBUG_RETURN(HA_ALTER_ERROR);
    } else if (GetIndexType(type) == 1) {
      g->Xchk= new(g) XCHK;
      PCHK xcp= (PCHK)g->Xchk;
  
      xcp->oldpix= GetIndexInfo(table->s);
      xcp->newpix= GetIndexInfo(altered_table->s);
      xcp->oldsep= GetBooleanOption("sepindex", false);
      xcp->oldsep= xcp->SetName(g, GetStringOption("optname"));
      tshp= altered_table->s;
      xcp->newsep= GetBooleanOption("sepindex", false);
      xcp->newsep= xcp->SetName(g, GetStringOption("optname"));
      tshp= NULL;
  
      if (xtrace && g->Xchk)
        htrc(
          "oldsep=%d newsep=%d oldopn=%s newopn=%s oldpix=%p newpix=%p\n",
                xcp->oldsep, xcp->newsep, 
                SVP(xcp->oldopn), SVP(xcp->newopn), 
                xcp->oldpix, xcp->newpix);
  
      if (sqlcom == SQLCOM_ALTER_TABLE)
        idx= true;
      else
        DBUG_RETURN(HA_ALTER_INPLACE_EXCLUSIVE_LOCK);

    } else if (!GetIndexType(type)) {
      sprintf(g->Message, "Table type %s is not indexable", oldopt->type);
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      DBUG_RETURN(HA_ALTER_ERROR);
    } // endif index type

    } // endif index operation

  if (!SameString(altered_table, "filename")) {
    if (!outward) {
      // Conversion to outward table is only allowed for file based
      // tables whose file does not exist.
      tshp= altered_table->s;
      char *fn= GetStringOption("filename");
      tshp= NULL;

      if (FileExists(fn, false)) {
        strcpy(g->Message, "Operation denied. Table data would be lost.");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        DBUG_RETURN(HA_ALTER_ERROR);
      } else
        goto fin;

    } else
      goto fin;

    } // endif filename

  /* Is there at least one operation that requires copy algorithm? */
  if (ha_alter_info->handler_flags & ~inplace_offline_operations)
    goto fin;

  /*
    ALTER TABLE tbl_name CONVERT TO CHARACTER SET .. and
    ALTER TABLE table_name DEFAULT CHARSET = .. most likely
    change column charsets and so not supported in-place through
    old API.

    Changing of PACK_KEYS, MAX_ROWS and ROW_FORMAT options were
    not supported as in-place operations in old API either.
  */
  if (create_info->used_fields & (HA_CREATE_USED_CHARSET |
                                  HA_CREATE_USED_DEFAULT_CHARSET |
                                  HA_CREATE_USED_PACK_KEYS |
                                  HA_CREATE_USED_MAX_ROWS) ||
      (table->s->row_type != create_info->row_type))
    goto fin;

#if 0
  uint table_changes= (ha_alter_info->handler_flags &
                       Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH) ?
    IS_EQUAL_PACK_LENGTH : IS_EQUAL_YES;

  if (table->file->check_if_incompatible_data(create_info, table_changes)
      == COMPATIBLE_DATA_YES)
    DBUG_RETURN(HA_ALTER_INPLACE_EXCLUSIVE_LOCK);
#endif // 0

  // This was in check_if_incompatible_data
  if (NoFieldOptionChange(altered_table) &&
      type == newtyp &&
      SameInt(altered_table, "lrecl") &&
      SameInt(altered_table, "elements") &&
      SameInt(altered_table, "header") &&
      SameInt(altered_table, "quoted") &&
      SameInt(altered_table, "ending") &&
      SameInt(altered_table, "compressed"))
    DBUG_RETURN(HA_ALTER_INPLACE_EXCLUSIVE_LOCK);

fin:
  if (idx) {
    // Indexing is only supported inplace
    my_message(ER_ALTER_OPERATION_NOT_SUPPORTED,
      "Alter operations not supported together by CONNECT", MYF(0));
    DBUG_RETURN(HA_ALTER_ERROR);
  } else if (outward) {
    if (IsFileType(type))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0,
        "This is an outward table, table data were not modified.");

    DBUG_RETURN(HA_ALTER_INPLACE_EXCLUSIVE_LOCK);
  } else
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

} // end of check_if_supported_inplace_alter


/**
  check_if_incompatible_data() called if ALTER TABLE can't detect otherwise
  if new and old definition are compatible

  @details If there are no other explicit signs like changed number of
  fields this function will be called by compare_tables()
  (sql/sql_tables.cc) to decide should we rewrite whole table or only .frm
  file.

  @note: This function is no more called by check_if_supported_inplace_alter
*/

bool ha_connect::check_if_incompatible_data(HA_CREATE_INFO *info,
                                        uint table_changes)
{
  DBUG_ENTER("ha_connect::check_if_incompatible_data");
  // TO DO: really implement and check it.
  push_warning(ha_thd(), Sql_condition::WARN_LEVEL_WARN, 0,
      "Unexpected call to check_if_incompatible_data.");
  DBUG_RETURN(COMPATIBLE_DATA_NO);
} // end of check_if_incompatible_data

/****************************************************************************
 * CONNECT MRR implementation: use DS-MRR
   This is just copied from myisam
 ***************************************************************************/

int ha_connect::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                     uint n_ranges, uint mode,
                                     HANDLER_BUFFER *buf)
{
  return ds_mrr.dsmrr_init(this, seq, seq_init_param, n_ranges, mode, buf);
} // end of multi_range_read_init

int ha_connect::multi_range_read_next(range_id_t *range_info)
{
  return ds_mrr.dsmrr_next(range_info);
} // end of multi_range_read_next

ha_rows ha_connect::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                               void *seq_init_param,
                                               uint n_ranges, uint *bufsz,
                                               uint *flags, Cost_estimate *cost)
{
  /*
    This call is here because there is no location where this->table would
    already be known.
    TODO: consider moving it into some per-query initialization call.
  */
  ds_mrr.init(this, table);

  // MMR is implemented for "local" file based tables only
  if (!IsFileType(GetRealType(GetTableOptionStruct())))
    *flags|= HA_MRR_USE_DEFAULT_IMPL;

  ha_rows rows= ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges,
                                        bufsz, flags, cost);
  xp->g->Mrr= !(*flags & HA_MRR_USE_DEFAULT_IMPL);
  return rows;
} // end of multi_range_read_info_const

ha_rows ha_connect::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                         uint key_parts, uint *bufsz,
                                         uint *flags, Cost_estimate *cost)
{
  ds_mrr.init(this, table);

  // MMR is implemented for "local" file based tables only
  if (!IsFileType(GetRealType(GetTableOptionStruct())))
    *flags|= HA_MRR_USE_DEFAULT_IMPL;

  ha_rows rows= ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz,
                                  flags, cost);
  xp->g->Mrr= !(*flags & HA_MRR_USE_DEFAULT_IMPL);
  return rows;
} // end of multi_range_read_info


int ha_connect::multi_range_read_explain_info(uint mrr_mode, char *str,
                                             size_t size)
{
  return ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
} // end of multi_range_read_explain_info

/* CONNECT MRR implementation ends */

#if 0
// Does this make sens for CONNECT?
Item *ha_connect::idx_cond_push(uint keyno_arg, Item* idx_cond_arg)
{
  pushed_idx_cond_keyno= keyno_arg;
  pushed_idx_cond= idx_cond_arg;
  in_range_check_pushed_down= TRUE;
  if (active_index == pushed_idx_cond_keyno)
    mi_set_index_cond_func(file, handler_index_cond_check, this);
  return NULL;
}
#endif // 0


struct st_mysql_storage_engine connect_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

/***********************************************************************/
/*  CONNECT global variables definitions.                              */
/***********************************************************************/
// Tracing: 0 no, 1 yes, >1 more tracing
static MYSQL_SYSVAR_INT(xtrace, xtrace,
       PLUGIN_VAR_RQCMDARG, "Console trace value.",
       NULL, update_connect_xtrace, 0, 0, INT_MAX, 1);

// Size used when converting TEXT columns to VARCHAR
static MYSQL_SYSVAR_INT(conv_size, conv_size,
       PLUGIN_VAR_RQCMDARG, "Size used when converting TEXT columns.",
       NULL, update_connect_zconv, SZCONV, 0, 65500, 1);

/**
  Type conversion:
    no:   Unsupported types -> TYPE_ERROR
    yes:  TEXT -> VARCHAR
    skip: skip unsupported type columns in Discovery
*/
const char *xconv_names[]=
{
  "NO", "YES", "SKIP", NullS
};

TYPELIB xconv_typelib=
{
  array_elements(xconv_names) - 1, "xconv_typelib",
  xconv_names, NULL
};

static MYSQL_SYSVAR_ENUM(
  type_conv,                       // name
  type_conv,                       // varname
  PLUGIN_VAR_RQCMDARG,             // opt
  "Unsupported types conversion.", // comment
  NULL,                            // check
  update_connect_xconv,            // update function
  0,                               // def (no)
  &xconv_typelib);                 // typelib

/**
  Temporary file usage:
    no:    Not using temporary file
    auto:  Using temporary file when needed
    yes:   Allways using temporary file
    force: Force using temporary file (no MAP)
    test:  Reserved
*/
const char *usetemp_names[]=
{
  "NO", "AUTO", "YES", "FORCE", "TEST", NullS
};

TYPELIB usetemp_typelib=
{
  array_elements(usetemp_names) - 1, "usetemp_typelib",
  usetemp_names, NULL
};

static MYSQL_SYSVAR_ENUM(
  use_tempfile,                    // name
  use_tempfile,                    // varname
  PLUGIN_VAR_RQCMDARG,             // opt
  "Temporary file use.",           // comment
  NULL,                            // check
  update_connect_usetemp,          // update function
  1,                               // def (AUTO)
  &usetemp_typelib);               // typelib

#if defined(XMAP)
// Using file mapping for indexes if true
static MYSQL_SYSVAR_BOOL(indx_map, indx_map, PLUGIN_VAR_RQCMDARG,
       "Using file mapping for indexes",
       NULL, update_connect_xmap, 0);
#endif   // XMAP

// Size used for g->Sarea_Size
static MYSQL_SYSVAR_UINT(work_size, work_size,
       PLUGIN_VAR_RQCMDARG, "Size of the CONNECT work area.",
       NULL, update_connect_worksize, SZWORK, SZWMIN, UINT_MAX, 1);

// Getting exact info values
static MYSQL_SYSVAR_BOOL(exact_info, exact_info, PLUGIN_VAR_RQCMDARG,
       "Getting exact info values",
       NULL, update_connect_xinfo, 0);

static struct st_mysql_sys_var* connect_system_variables[]= {
  MYSQL_SYSVAR(xtrace),
  MYSQL_SYSVAR(conv_size),
  MYSQL_SYSVAR(type_conv),
#if defined(XMAP)
  MYSQL_SYSVAR(indx_map),
#endif   // XMAP
  MYSQL_SYSVAR(work_size),
  MYSQL_SYSVAR(use_tempfile),
  MYSQL_SYSVAR(exact_info),
  NULL
};

maria_declare_plugin(connect)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &connect_storage_engine,
  "CONNECT",
  "Olivier Bertrand",
  "Management of External Data (SQL/MED), including many file formats",
  PLUGIN_LICENSE_GPL,
  connect_init_func,                            /* Plugin Init */
  connect_done_func,                            /* Plugin Deinit */
  0x0103,                                       /* version number (1.03) */
  NULL,                                         /* status variables */
  connect_system_variables,                     /* system variables */
  "1.03",                                       /* string version */
  MariaDB_PLUGIN_MATURITY_BETA                  /* maturity */
}
maria_declare_plugin_end;
