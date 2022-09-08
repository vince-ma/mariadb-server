/*
   Copyright (c) 2019, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* !!! For inclusion into ha_federatedx.cc */


/*
  This is a quick a dirty implemention of the derived_handler and select_handler
  interfaces to be used to push select queries and the queries specifying
  derived tables into FEDERATEDX engine.
  The functions
    create_federatedx_derived_handler and
    create_federatedx_select_handler
  that return the corresponding interfaces for pushdown capabilities do
  not check a lot of things. In particular they do not check that the tables
  of the pushed queries belong to the same foreign server.

  The implementation is provided purely for testing purposes.
  The pushdown capabilities are enabled by turning on the plugin system
  variable federated_pushdown:
    set global federated_pushdown=1;
*/


static std::pair<handlerton *, TABLE *> get_handlerton(SELECT_LEX *sel_lex)
{
  handlerton *hton= nullptr;
  TABLE *table= nullptr;
  if (!sel_lex->join)
    return {nullptr, nullptr};
  for (TABLE_LIST *tbl= sel_lex->join->tables_list; tbl; tbl= tbl->next_local)
  {
    if (!tbl->table)
      return {nullptr, nullptr};
    if (!hton)
    {
      hton= tbl->table->file->partition_ht();
      table= tbl->table;
    }
    else if (hton != tbl->table->file->partition_ht())
      return {nullptr, nullptr};
  }

  for (SELECT_LEX_UNIT *un= sel_lex->first_inner_unit(); un;
       un= un->next_unit())
  {
    for (SELECT_LEX *sl= un->first_select(); sl; sl= sl->next_select())
    {
      auto inner_ht= get_handlerton(sl);
      if (!hton)
      {
        hton= inner_ht.first;
        table= inner_ht.second;
      }
      else if (hton != inner_ht.first)
        return {nullptr, nullptr};
    }
  }
  return {hton, table};
}


/*
  Check that all tables in the lex_unit use the same storage engine.

  @return
    the storage engine's handlerton and an example table.

  @todo
    Why does this need to be so generic? We know we need
    tables with hton == federatedx_hton, why not only look for
    those tables?
*/
static std::pair<handlerton *, TABLE *>
get_handlerton_for_unit(SELECT_LEX_UNIT *lex_unit)
{
  handlerton *hton= nullptr;
  TABLE *table= nullptr;
  for (auto sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    auto next_ht= get_handlerton(sel_lex);
    if (!hton)
    {
      hton= next_ht.first;
      table= next_ht.second;
    }
    else if (hton != next_ht.first)
      return {nullptr, nullptr};
  }
  return {hton, table};
}


static derived_handler*
create_federatedx_derived_handler(THD* thd, TABLE_LIST *derived)
{
  if (!use_pushdown)
    return 0;

  SELECT_LEX_UNIT *unit= derived->derived;

  auto hton= get_handlerton_for_unit(unit);
  if (!hton.first)
    return nullptr;

  return new ha_federatedx_derived_handler(thd, derived, hton.second);
}


/*
  Implementation class of the derived_handler interface for FEDERATEDX:
  class implementation
*/

ha_federatedx_derived_handler::ha_federatedx_derived_handler(THD *thd,
                                                             TABLE_LIST *dt,
                                                             TABLE *tbl)
  : derived_handler(thd, federatedx_hton),
    federatedx_handler_base(thd, tbl)
{
  derived= dt;

  query.length(0);
  dt->derived->print(&query,
                     enum_query_type(QT_VIEW_INTERNAL |
                                     QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                     QT_PARSABLE));
}


int federatedx_handler_base::end_scan_()
{
  DBUG_ENTER("ha_federatedx_derived_handler::end_scan");

  (*iop)->free_result(stored_result);

  free_share(txn, share);

  DBUG_RETURN(0);
}

void ha_federatedx_derived_handler::print_error(int, unsigned long)
{
}


static select_handler *create_federatedx_select_handler(
  THD *thd, SELECT_LEX *sel_lex)
{
  if (!use_pushdown)
    return nullptr;

  auto hton= get_handlerton(sel_lex);
  if (!hton.first)
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return NULL;

  return new ha_federatedx_select_handler(thd, sel_lex, hton.second);
}

static select_handler *create_federatedx_unit_handler(
  THD* thd, SELECT_LEX_UNIT *sel_unit)
{
  if (!use_pushdown)
    return nullptr;

  auto hton= get_handlerton_for_unit(sel_unit);
  if (!hton.first)
    return nullptr;

  if (sel_unit->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_federatedx_select_handler(thd, sel_unit, hton.second);
}

/*
  Implementation class of the select_handler interface for FEDERATEDX:
  class implementation
*/

federatedx_handler_base::federatedx_handler_base(THD *thd_arg, TABLE *tbl_arg)
 : share(NULL), txn(NULL), iop(NULL), stored_result(NULL),
   query(thd_arg->charset()),
   query_table(tbl_arg)
{}

ha_federatedx_select_handler::ha_federatedx_select_handler(
    THD *thd, SELECT_LEX *select_lex, TABLE *tbl)
  : select_handler(thd, federatedx_hton, select_lex),
    federatedx_handler_base(thd, tbl)
{
  query.length(0);
  select_lex->print(thd, &query,
                    enum_query_type(QT_VIEW_INTERNAL |
                                    QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                    QT_PARSABLE));
}


ha_federatedx_select_handler::ha_federatedx_select_handler(
    THD *thd, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd, federatedx_hton, lex_unit), 
    federatedx_handler_base(thd, tbl)
{
  query.length(0);
  lex_unit->print(&query,
                  enum_query_type(QT_VIEW_INTERNAL |
                                  QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                  QT_PARSABLE));
}


int federatedx_handler_base::init_scan_()
{
  THD *thd= query_table->in_use;
  int rc= 0;

  DBUG_ENTER("ha_federatedx_select_handler::init_scan");

  ha_federatedx *h= (ha_federatedx *) query_table->file;
  iop= &h->io;
  share= get_share(query_table->s->table_name.str, query_table);
  txn= h->get_txn(thd);
  if ((rc= txn->acquire(share, thd, TRUE, iop)))
    DBUG_RETURN(rc);

  if ((*iop)->query(query.ptr(), query.length()))
    goto err;

  stored_result= (*iop)->store_result();
  if (!stored_result)
      goto err;

  DBUG_RETURN(0);

err:
  DBUG_RETURN(HA_FEDERATEDX_ERROR_WITH_REMOTE_SYSTEM);
}

int federatedx_handler_base::next_row_(TABLE *table)
{
  int rc= 0;
  FEDERATEDX_IO_ROW *row;
  ulong *lengths;
  Field **field;
  int column= 0;
  Time_zone *saved_time_zone= table->in_use->variables.time_zone;
  DBUG_ENTER("ha_federatedx_select_handler::next_row");

  if ((rc= txn->acquire(share, table->in_use, TRUE, iop)))
    DBUG_RETURN(rc);

  if (!(row= (*iop)->fetch_row(stored_result)))
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  /* Convert row to internal format */
  table->in_use->variables.time_zone= UTC;
  lengths= (*iop)->fetch_lengths(stored_result);

  for (field= table->field; *field; field++, column++)
  {
    if ((*iop)->is_column_null(row, column))
       (*field)->set_null();
    else
    {
      (*field)->set_notnull();
      (*field)->store((*iop)->get_column_data(row, column),
                      lengths[column], &my_charset_bin);
    }
  }
  table->in_use->variables.time_zone= saved_time_zone;

  DBUG_RETURN(rc);
}

int ha_federatedx_select_handler::end_scan()
{
  free_tmp_table(thd, table);
  table= 0;

  return federatedx_handler_base::end_scan_();
}

void ha_federatedx_select_handler::print_error(int error, myf error_flag)
{
  select_handler::print_error(error, error_flag);
}
