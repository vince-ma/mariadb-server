/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file ibuf/ibuf0ibuf.cc
Insert buffer

Created 7/19/1997 Heikki Tuuri
*******************************************************/

#include "ibuf0ibuf.h"
#include "page0page.h"
#include "log.h"

constexpr const page_id_t ibuf_root{0, FSP_IBUF_TREE_ROOT_PAGE_NO};
constexpr const page_id_t ibuf_header{0, FSP_IBUF_HEADER_PAGE_NO};
constexpr const index_id_t ibuf_index_id{0xFFFFFFFF00000000ULL};

/** Removes a page from the free list and frees it to the fsp system.
@return error code
@retval DB_SUCCESS            if more work may remain to be done
@retval DB_SUCCESS_LOCKED_REC if everything was freed */
ATTRIBUTE_COLD static dberr_t ibuf_remove_free_page()
{
  mtr_t mtr;

  log_free_check();

  mtr.start();

  mtr.x_lock_space(fil_system.sys_space);
  dberr_t err;
  buf_block_t* header= buf_page_get_gen(ibuf_header, 0, RW_X_LATCH, nullptr,
                                        BUF_GET, &mtr, &err);

  if (!header)
  {
func_exit:
    mtr.commit();
    return err;
  }

  buf_block_t *root= buf_page_get_gen(ibuf_root, 0, RW_X_LATCH,
                                      nullptr, BUF_GET, &mtr, &err);

  if (UNIV_UNLIKELY(!root))
    goto func_exit;

  const uint32_t page_no= flst_get_last(PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST +
                                        root->page.frame).page;
  if (page_no == FIL_NULL)
  {
    mtr.set_modified(*root);
    fsp_init_file_page(fil_system.sys_space, root, &mtr);
    err= DB_SUCCESS_LOCKED_REC;
    goto func_exit;
  }

  /* Since pessimistic inserts were prevented, we know that the
  page is still in the free list. NOTE that also deletes may take
  pages from the free list, but they take them from the start, and
  the free list was so long that they cannot have taken the last
  page from it. */

  err= fseg_free_page(header->page.frame + PAGE_DATA, fil_system.sys_space,
                      page_no, &mtr);

  if (err != DB_SUCCESS)
    goto func_exit;

  if (page_no != flst_get_last(PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST +
                               root->page.frame).page)
  {
    err= DB_CORRUPTION;
    goto func_exit;
  }

  /* Remove the page from the free list and update the ibuf size data */
  if (buf_block_t *block=
      buf_page_get_gen(page_id_t{0, page_no}, 0, RW_X_LATCH, nullptr, BUF_GET,
                       &mtr, &err))
    err= flst_remove(root, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
                     block, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, &mtr);

  if (err == DB_SUCCESS)
    buf_page_free(fil_system.sys_space, page_no, &mtr);

  goto func_exit;
}

ATTRIBUTE_COLD static dberr_t ibuf_upgrade()
{
  if (srv_read_only_mode)
  {
    sql_print_error("InnoDB: innodb_read_only_mode prevents an upgrade");
    return DB_READ_ONLY;
  }

  sql_print_information("InnoDB: Upgrading the change buffer");

  for (;;)
  {
    if (dberr_t err= ibuf_remove_free_page())
    {
      if (err == DB_SUCCESS_LOCKED_REC)
        break;
      sql_print_error("InnoDB: Unable to upgrade the change buffer");
      return err;
    }
  }

  sql_print_information("InnoDB: Removed the change buffer");
  return DB_SUCCESS;
}

dberr_t ibuf_cleanup()
{
  mtr_t mtr;
  mtr.start();
  mtr.x_lock_space(fil_system.sys_space);
  dberr_t err;
  buf_block_t *header_page=
    buf_page_get_gen(ibuf_header, 0, RW_X_LATCH, nullptr, BUF_GET, &mtr, &err);

  if (!header_page)
  {
  err_exit:
    sql_print_error("InnoDB: The change buffer is corrupted");
  func_exit:
    mtr.commit();
    return err;
  }

  buf_block_t *root= buf_page_get_gen(ibuf_root, 0, RW_X_LATCH, nullptr,
                                      BUF_GET, &mtr, &err);
  if (!root)
    goto err_exit;

  if (UNIV_LIKELY(!page_has_siblings(root->page.frame)) &&
      UNIV_LIKELY(!memcmp(root->page.frame + FIL_PAGE_TYPE, field_ref_zero,
                          srv_page_size -
                          (FIL_PAGE_DATA_END + FIL_PAGE_TYPE))))
    goto func_exit; // the change buffer was removed; no need to upgrade

  if (page_is_comp(root->page.frame) ||
      fil_page_get_type(root->page.frame) != FIL_PAGE_INDEX ||
      mach_read_from_8(root->page.frame + PAGE_HEADER + PAGE_INDEX_ID) !=
      ibuf_index_id)
  {
    err= DB_CORRUPTION;
    goto err_exit;
  }

  if (!page_is_empty(root->page.frame))
  {
    sql_print_error("The change buffer is not empty! Please start up"
                    " MariaDB 10.8 or later and shut it down after"
                    " SET GLOBAL innodb_fast_shutdown=0");
    err= DB_FAIL;
    goto func_exit;
  }

  mtr.commit();
  return ibuf_upgrade();
}
