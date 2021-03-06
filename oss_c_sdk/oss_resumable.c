#include "aos_log.h"
#include "aos_define.h"
#include "aos_util.h"
#include "aos_string.h"
#include "aos_status.h"
#include "oss_auth.h"
#include "oss_util.h"
#include "oss_xml.h"
#include "oss_api.h"
#include "oss_resumable.h"

int32_t oss_get_thread_num(oss_resumable_clt_params_t *clt_params)
{
    if ((NULL == clt_params) || (clt_params->thread_num <= 0 || clt_params->thread_num > 1024)) {
        return 1;
    }
    return clt_params->thread_num;
}

void oss_get_checkpoint_path(oss_resumable_clt_params_t *clt_params, const aos_string_t *filepath, 
                             aos_pool_t *pool, aos_string_t *checkpoint_path)
{
    if ((NULL == checkpoint_path) || (NULL == clt_params) || (!clt_params->enable_checkpoint)) {
        return;
    }

    if (aos_is_null_string(&clt_params->checkpoint_path)) {
        int len = filepath->len + strlen(".cp") + 1;
        char *buffer = (char *)aos_pcalloc(pool, len);
        apr_snprintf(buffer, len, "%.*s.cp", filepath->len, filepath->data);
        aos_str_set(checkpoint_path , buffer);
        return;
    }

    checkpoint_path->data = clt_params->checkpoint_path.data;
    checkpoint_path->len = clt_params->checkpoint_path.len;
}

int oss_get_file_info(const aos_string_t *filepath, aos_pool_t *pool, apr_finfo_t *finfo) 
{
    apr_status_t s;
    char buf[256];
    apr_file_t *thefile;

    s = apr_file_open(&thefile, filepath->data, APR_READ, APR_UREAD | APR_GREAD, pool);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_open failure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return s;
    }

    s = apr_file_info_get(finfo, APR_FINFO_NORM, thefile);
    if (s != APR_SUCCESS) {
        apr_file_close(thefile);
        aos_error_log("apr_file_info_get failure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return s;
    }
    apr_file_close(thefile);

    return AOSE_OK;
}

int oss_does_file_exist(const aos_string_t *filepath, aos_pool_t *pool) 
{
    apr_status_t s;
    apr_file_t *thefile;

    s = apr_file_open(&thefile, filepath->data, APR_READ, APR_UREAD | APR_GREAD, pool);
    if (s != APR_SUCCESS) {
        return AOS_FALSE;
    }

    apr_file_close(thefile);
    return AOS_TRUE;
}

int oss_open_checkpoint_file(aos_pool_t *pool,  aos_string_t *checkpoint_path, oss_checkpoint_t *checkpoint) 
{
    apr_status_t s;
    apr_file_t *thefile;
    char buf[256];
    s = apr_file_open(&thefile, checkpoint_path->data, APR_CREATE | APR_WRITE, APR_UREAD | APR_UWRITE | APR_GREAD, pool);
    if (s == APR_SUCCESS) {
        aos_error_log("apr_file_info_get failure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        checkpoint->thefile = thefile;
    }
    return s;
}

int oss_get_part_num(int64_t file_size, int64_t part_size)
{
    int64_t num = 0;
    int64_t left = 0;
    left = (file_size % part_size == 0) ? 0 : 1;
    num = file_size / part_size + left;
    return (int)num;
}

void oss_build_parts(int64_t file_size, int64_t part_size, oss_checkpoint_part_t *parts)
{
    int i = 0;
    for (; i * part_size < file_size; i++) {
        parts[i].index = i;
        parts[i].offset = i * part_size;
        parts[i].size = aos_min(part_size, (file_size - i * part_size));
        parts[i].completed = AOS_FALSE;
    }
}

void oss_build_thread_params(oss_upload_thread_params_t *thr_params, int part_num, 
                             aos_pool_t *parent_pool, oss_request_options_t *options, 
                             aos_string_t *bucket, aos_string_t *object, aos_string_t *filepath,
                             aos_string_t *upload_id, oss_checkpoint_part_t *parts,
                             oss_part_task_result_t *result) 
{
    int i = 0;
    aos_pool_t *subpool = NULL;
    oss_config_t *config = NULL;
    aos_http_controller_t *ctl;
    for (; i < part_num; i++) {
        aos_pool_create(&subpool, parent_pool); 
        config = oss_config_create(subpool);
        aos_str_set(&config->endpoint, options->config->endpoint.data);
        aos_str_set(&config->access_key_id, options->config->access_key_id.data);
        aos_str_set(&config->access_key_secret, options->config->access_key_secret.data);
        config->is_cname = options->config->is_cname;
        ctl = aos_http_controller_create(subpool, 0);
        thr_params[i].options.config = config;
        thr_params[i].options.ctl = ctl;
        thr_params[i].options.pool = subpool;
        thr_params[i].bucket = bucket;
        thr_params[i].object = object;
        thr_params[i].filepath = filepath;
        thr_params[i].upload_id = upload_id;
        thr_params[i].part = parts + i;
        thr_params[i].result = result + i;
        thr_params[i].result->part = thr_params[i].part;
    }
}

void oss_destroy_thread_pool(oss_upload_thread_params_t *thr_params, int part_num) 
{
    int i = 0;
    for (; i < part_num; i++) {
        aos_pool_destroy(thr_params[i].options.pool);
    }
}

void oss_set_task_tracker(oss_upload_thread_params_t *thr_params, int part_num, 
                          apr_uint32_t *launched, apr_uint32_t *failed, apr_uint32_t *completed,
                          apr_queue_t *failed_parts, apr_queue_t *completed_parts) 
{
    int i = 0;
    for (; i < part_num; i++) {
        thr_params[i].launched = launched;
        thr_params[i].failed = failed;
        thr_params[i].completed = completed;
        thr_params[i].failed_parts = failed_parts;
        thr_params[i].completed_parts = completed_parts;
    }
}

int oss_verify_checkpoint_md5(aos_pool_t *pool, const oss_checkpoint_t *checkpoint)
{
    return AOS_TRUE;
}

void oss_build_upload_checkpoint(aos_pool_t *pool, oss_checkpoint_t *checkpoint, aos_string_t *file_path, 
                                 apr_finfo_t *finfo, aos_string_t *upload_id, int64_t part_size) 
{
    int i = 0;

    checkpoint->cp_type = OSS_CP_UPLOAD;
    aos_str_set(&checkpoint->file_path, aos_pstrdup(pool, file_path));
    checkpoint->file_size = finfo->size;
    checkpoint->file_last_modified = finfo->mtime;
    aos_str_set(&checkpoint->upload_id, aos_pstrdup(pool, upload_id));

    checkpoint->part_size = part_size;
    for (; i * part_size < finfo->size; i++) {
        checkpoint->parts[i].index = i;
        checkpoint->parts[i].offset = i * part_size;
        checkpoint->parts[i].size = aos_min(part_size, (finfo->size - i * part_size));
        checkpoint->parts[i].completed = AOS_FALSE;
        aos_str_set(&checkpoint->parts[i].etag , "");
    }
    checkpoint->part_num = i;
}

int oss_dump_checkpoint(aos_pool_t *pool, const oss_checkpoint_t *checkpoint) 
{
    char *xml_body = NULL;
    apr_status_t s;
    char buf[256];
    apr_size_t len;
    
    // to xml
    xml_body = oss_build_checkpoint_xml(pool, checkpoint);
    if (NULL == xml_body) {
        return AOSE_OUT_MEMORY;
    }

    // truncate to empty
    s = apr_file_trunc(checkpoint->thefile, 0);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_write fialure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return AOSE_FILE_TRUNC_ERROR;
    }
   
    // write to file
    len = strlen(xml_body);
    s = apr_file_write(checkpoint->thefile, xml_body, &len);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_write fialure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return AOSE_FILE_WRITE_ERROR;
    }

    // flush file
    s = apr_file_flush(checkpoint->thefile);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_flush fialure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return AOSE_FILE_FLUSH_ERROR;
    }

    return AOSE_OK;
}

int oss_load_checkpoint(aos_pool_t *pool, const aos_string_t *filepath, oss_checkpoint_t *checkpoint) 
{
    apr_status_t s;
    char buf[256];
    apr_size_t len;
    apr_finfo_t finfo;
    char *xml_body = NULL;
    apr_file_t *thefile;

    // open file
    s = apr_file_open(&thefile, filepath->data, APR_READ, APR_UREAD | APR_GREAD, pool);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_open failure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        return AOSE_OPEN_FILE_ERROR;
    }

    // get file stat
    s = apr_file_info_get(&finfo, APR_FINFO_NORM, thefile);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_info_get failure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        apr_file_close(thefile);
        return AOSE_FILE_INFO_ERROR;
    }

    xml_body = (char *)aos_palloc(pool, (apr_size_t)(finfo.size + 1));

    // read
    s = apr_file_read_full(thefile, xml_body, (apr_size_t)finfo.size, &len);
    if (s != APR_SUCCESS) {
        aos_error_log("apr_file_read_full fialure, code:%d %s.", s, apr_strerror(s, buf, sizeof(buf)));
        apr_file_close(thefile);
        return AOSE_FILE_READ_ERROR;
    }
    apr_file_close(thefile);
    xml_body[len] = '\0';

    // parse
    return oss_checkpoint_parse_from_body(pool, xml_body, checkpoint);
}

int oss_is_upload_checkpoint_valid(aos_pool_t *pool, oss_checkpoint_t *checkpoint, apr_finfo_t *finfo)
{
    if (oss_verify_checkpoint_md5(pool, checkpoint) && 
        (checkpoint->file_size == finfo->size) && 
        (checkpoint->file_last_modified == finfo->mtime)) {
        return AOS_TRUE;
    }
    return AOS_FALSE;
}

void oss_update_checkpoint(aos_pool_t *pool, oss_checkpoint_t *checkpoint, int32_t part_index, aos_string_t *etag) 
{
    char *p = NULL;
    checkpoint->parts[part_index].completed = AOS_TRUE;
    p = apr_pstrdup(pool, etag->data);
    aos_str_set(&checkpoint->parts[part_index].etag, p);
}

void oss_get_checkpoint_undo_parts(oss_checkpoint_t *checkpoint, int *part_num, oss_checkpoint_part_t *parts)
{
    int i = 0;
    int idx = 0;
    for (; i < checkpoint->part_num; i++) {
        if (!checkpoint->parts[i].completed) {
            parts[idx].index = checkpoint->parts[i].index;
            parts[idx].offset = checkpoint->parts[i].offset;
            parts[idx].size = checkpoint->parts[i].size;
            parts[idx].completed = checkpoint->parts[i].completed;
            idx++;
        }
    }
    *part_num = idx;
}

void * APR_THREAD_FUNC upload_part(apr_thread_t *thd, void *data) 
{
    aos_status_t *s = NULL;
    oss_upload_thread_params_t *params = NULL;
    oss_upload_file_t *upload_file = NULL;
    aos_table_t *resp_headers = NULL;
    int part_num;
    char *etag;
    
    params = (oss_upload_thread_params_t *)data;
    if (apr_atomic_read32(params->failed) > 0) {
        apr_atomic_inc32(params->launched);
        return NULL;
    }

    part_num = params->part->index + 1;
    upload_file = oss_create_upload_file(params->options.pool);
    aos_str_set(&upload_file->filename, params->filepath->data);
    upload_file->file_pos = params->part->offset;
    upload_file->file_last = params->part->offset + params->part->size;

    s = oss_upload_part_from_file(&params->options, params->bucket, params->object, params->upload_id,
        part_num, upload_file, &resp_headers);
    if (!aos_status_is_ok(s)) {
        apr_atomic_inc32(params->failed);
        params->result->s = s;
        apr_queue_push(params->failed_parts, params->result);
        return s;
    }

    etag = apr_pstrdup(params->options.pool, (char*)apr_table_get(resp_headers, "ETag"));
    aos_str_set(&params->result->etag, etag);
    apr_atomic_inc32(params->completed);
    apr_queue_push(params->completed_parts, params->result);
    return NULL;
}

aos_status_t *oss_resumable_upload_file_without_cp(oss_request_options_t *options,
                                                   aos_string_t *bucket, 
                                                   aos_string_t *object, 
                                                   aos_string_t *filepath,                           
                                                   aos_table_t *headers,
                                                   aos_table_t *params,
                                                   int32_t thread_num,
                                                   int64_t part_size,
                                                   apr_finfo_t *finfo,
                                                   oss_progress_callback progress_callback,
                                                   aos_table_t **resp_headers,
                                                   aos_list_t *resp_body) 
{
    aos_pool_t *subpool = NULL;
    aos_pool_t *parent_pool = NULL;
    aos_status_t *s = NULL;
    aos_status_t *ret = NULL;
    aos_list_t completed_part_list;
    oss_complete_part_content_t *complete_content = NULL;
    aos_string_t upload_id;
    oss_checkpoint_part_t *parts;
    oss_part_task_result_t *results;
    oss_part_task_result_t *task_res;
    oss_upload_thread_params_t *thr_params;
    aos_table_t *cb_headers = NULL;
    apr_thread_pool_t *thrp;
    apr_uint32_t launched = 0;
    apr_uint32_t failed = 0;
    apr_uint32_t completed = 0;
    apr_uint32_t total_num = 0;
    apr_queue_t *failed_parts;
    apr_queue_t *completed_parts;
    int64_t consume_bytes = 0;
    void *task_result;
    char *part_num_str;
    char *etag;
    int part_num = 0;
    int i = 0;
    int rv;

    // prepare
    parent_pool = options->pool;
    ret = aos_status_create(parent_pool);
    part_num = oss_get_part_num(finfo->size, part_size);
    parts = (oss_checkpoint_part_t *)aos_palloc(parent_pool, sizeof(oss_checkpoint_part_t) * part_num);
    oss_build_parts(finfo->size, part_size, parts);
    results = (oss_part_task_result_t *)aos_palloc(parent_pool, sizeof(oss_part_task_result_t) * part_num);
    thr_params = (oss_upload_thread_params_t *)aos_palloc(parent_pool, sizeof(oss_upload_thread_params_t) * part_num);
    oss_build_thread_params(thr_params, part_num, parent_pool, options, bucket, object, filepath, &upload_id, parts, results);
    
    // init upload
    aos_pool_create(&subpool, parent_pool);
    options->pool = subpool;
    s = oss_init_multipart_upload(options, bucket, object, &upload_id, headers, resp_headers);
    if (!aos_status_is_ok(s)) {
        s = aos_status_dup(parent_pool, s);
        aos_pool_destroy(subpool);
        options->pool = parent_pool;
        return s;
    }
    aos_str_set(&upload_id, apr_pstrdup(parent_pool, upload_id.data));
    options->pool = parent_pool;
    aos_pool_destroy(subpool);

    // upload parts    
    rv = apr_thread_pool_create(&thrp, 0, thread_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_THREAD_POOL_ERROR_CODE, NULL); 
        return ret;
    }

    rv = apr_queue_create(&failed_parts, part_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_QUEUE_ERROR_CODE, NULL); 
        return ret;
    }

    rv = apr_queue_create(&completed_parts, part_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_QUEUE_ERROR_CODE, NULL); 
        return ret;
    }

    // launch
    oss_set_task_tracker(thr_params, part_num, &launched, &failed, &completed, failed_parts, completed_parts);
    for (i = 0; i < part_num; i++) {
        apr_thread_pool_push(thrp, upload_part, thr_params + i, 0, NULL);
    }

    // wait until all tasks exit
    total_num = apr_atomic_read32(&launched) + apr_atomic_read32(&failed) + apr_atomic_read32(&completed);
    for ( ; total_num < (apr_uint32_t)part_num; ) {
        rv = apr_queue_trypop(completed_parts, &task_result);
        if (rv == APR_EINTR || rv == APR_EAGAIN) {
            apr_sleep(1000);
        } else if(rv == APR_EOF) {
            break;
        } else if(rv == APR_SUCCESS) {
            task_res = (oss_part_task_result_t*)task_result;
            if (NULL != progress_callback) {
                consume_bytes += task_res->part->size;
                progress_callback(consume_bytes, finfo->size);
            }
        }
        total_num = apr_atomic_read32(&launched) + apr_atomic_read32(&failed) + apr_atomic_read32(&completed);
    }

    // deal with left successful parts
    while(APR_SUCCESS == apr_queue_trypop(completed_parts, &task_result)) {
        task_res = (oss_part_task_result_t*)task_result;
        if (NULL != progress_callback) {
            consume_bytes += task_res->part->size;
            progress_callback(consume_bytes, finfo->size);
        }
    }

    // failed
    if (apr_atomic_read32(&failed) > 0) {
        apr_queue_pop(failed_parts, &task_result);
        task_res = (oss_part_task_result_t*)task_result;
        s = aos_status_dup(parent_pool, task_res->s);
        oss_destroy_thread_pool(thr_params, part_num);
        return s;
    }

    // successful
    aos_pool_create(&subpool, parent_pool);
    aos_list_init(&completed_part_list);
    for (i = 0; i < part_num; i++) {
        complete_content = oss_create_complete_part_content(subpool);
        part_num_str = apr_psprintf(subpool, "%d", thr_params[i].part->index + 1);
        aos_str_set(&complete_content->part_number, part_num_str);
        etag = apr_pstrdup(subpool, thr_params[i].result->etag.data);
        aos_str_set(&complete_content->etag, etag);
        aos_list_add_tail(&complete_content->node, &completed_part_list);
    }
    oss_destroy_thread_pool(thr_params, part_num);

    // complete upload
    options->pool = subpool;
    if (NULL != headers && NULL != apr_table_get(headers, OSS_CALLBACK)) {
        cb_headers = aos_table_make(subpool, 2);
        apr_table_set(cb_headers, OSS_CALLBACK, apr_table_get(headers, OSS_CALLBACK));
        if (NULL != apr_table_get(headers, OSS_CALLBACK_VAR)) {
            apr_table_set(cb_headers, OSS_CALLBACK_VAR, apr_table_get(headers, OSS_CALLBACK_VAR));
        }
    }
    s = oss_do_complete_multipart_upload(options, bucket, object, &upload_id, 
        &completed_part_list, cb_headers, NULL, resp_headers, resp_body);
    s = aos_status_dup(parent_pool, s);
    aos_pool_destroy(subpool);
    options->pool = parent_pool;

    return s;
}

aos_status_t *oss_resumable_upload_file_with_cp(oss_request_options_t *options,
                                                aos_string_t *bucket, 
                                                aos_string_t *object, 
                                                aos_string_t *filepath,                           
                                                aos_table_t *headers,
                                                aos_table_t *params,
                                                int32_t thread_num,
                                                int64_t part_size,
                                                aos_string_t *checkpoint_path,
                                                apr_finfo_t *finfo,
                                                oss_progress_callback progress_callback,
                                                aos_table_t **resp_headers,
                                                aos_list_t *resp_body) 
{
    aos_pool_t *subpool = NULL;
    aos_pool_t *parent_pool = NULL;
    aos_status_t *s = NULL;
    aos_status_t *ret = NULL;
    aos_list_t completed_part_list;
    oss_complete_part_content_t *complete_content = NULL;
    aos_string_t upload_id;
    oss_checkpoint_part_t *parts;
    oss_part_task_result_t *results;
    oss_part_task_result_t *task_res;
    oss_upload_thread_params_t *thr_params;
    aos_table_t *cb_headers = NULL;
    apr_thread_pool_t *thrp;
    apr_uint32_t launched = 0;
    apr_uint32_t failed = 0;
    apr_uint32_t completed = 0;
    apr_uint32_t total_num = 0;
    apr_queue_t *failed_parts;
    apr_queue_t *completed_parts;
    oss_checkpoint_t *checkpoint = NULL;
    int need_init_upload = AOS_TRUE;
    int has_left_result = AOS_FALSE;
    int64_t consume_bytes = 0;
    void *task_result;
    char *part_num_str;
    int part_num = 0;
    int i = 0;
    int rv;

    // checkpoint
    parent_pool = options->pool;
    ret = aos_status_create(parent_pool);
    checkpoint = oss_create_checkpoint_content(parent_pool);
    if(oss_does_file_exist(checkpoint_path, parent_pool)) {
        if (AOSE_OK == oss_load_checkpoint(parent_pool, checkpoint_path, checkpoint) && 
            oss_is_upload_checkpoint_valid(parent_pool, checkpoint, finfo)) {
                aos_str_set(&upload_id, checkpoint->upload_id.data);
                need_init_upload = AOS_FALSE;
        } else {
            apr_file_remove(checkpoint_path->data, parent_pool);
        }
    }

    if (need_init_upload) {
        // init upload 
        aos_pool_create(&subpool, parent_pool);
        options->pool = subpool;
        s = oss_init_multipart_upload(options, bucket, object, &upload_id, headers, resp_headers);
        if (!aos_status_is_ok(s)) {
            s = aos_status_dup(parent_pool, s);
            aos_pool_destroy(subpool);
            options->pool = parent_pool;
            return s;
        }
        aos_str_set(&upload_id, apr_pstrdup(parent_pool, upload_id.data));
        options->pool = parent_pool;
        aos_pool_destroy(subpool);

        // build checkpoint
        oss_build_upload_checkpoint(parent_pool, checkpoint, filepath, finfo, &upload_id, part_size);
    }

    rv = oss_open_checkpoint_file(parent_pool, checkpoint_path, checkpoint);
    if (rv != APR_SUCCESS) {
        aos_status_set(ret, rv, AOS_OPEN_FILE_ERROR_CODE, NULL);
        return ret;
    }

    // prepare
    ret = aos_status_create(parent_pool);
    parts = (oss_checkpoint_part_t *)aos_palloc(parent_pool, sizeof(oss_checkpoint_part_t) * (checkpoint->part_num));
    oss_get_checkpoint_undo_parts(checkpoint, &part_num, parts);
    results = (oss_part_task_result_t *)aos_palloc(parent_pool, sizeof(oss_part_task_result_t) * part_num);
    thr_params = (oss_upload_thread_params_t *)aos_palloc(parent_pool, sizeof(oss_upload_thread_params_t) * part_num);
    oss_build_thread_params(thr_params, part_num, parent_pool, options, bucket, object, filepath, &upload_id, parts, results);

    // upload parts    
    rv = apr_thread_pool_create(&thrp, 0, thread_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_THREAD_POOL_ERROR_CODE, NULL); 
        return ret;
    }

    rv = apr_queue_create(&failed_parts, part_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_QUEUE_ERROR_CODE, NULL); 
        return ret;
    }

    rv = apr_queue_create(&completed_parts, part_num, parent_pool);
    if (APR_SUCCESS != rv) {
        aos_status_set(ret, rv, AOS_CREATE_QUEUE_ERROR_CODE, NULL); 
        return ret;
    }

    // launch
    oss_set_task_tracker(thr_params, part_num, &launched, &failed, &completed, failed_parts, completed_parts);
    for (i = 0; i < part_num; i++) {
        apr_thread_pool_push(thrp, upload_part, thr_params + i, 0, NULL);
    }

    // wait until all tasks exit
    total_num = apr_atomic_read32(&launched) + apr_atomic_read32(&failed) + apr_atomic_read32(&completed);
    for ( ; total_num < (apr_uint32_t)part_num; ) {
        rv = apr_queue_trypop(completed_parts, &task_result);
        if (rv == APR_EINTR || rv == APR_EAGAIN) {
            apr_sleep(1000);
        } else if(rv == APR_EOF) {
            break;
        } else if(rv == APR_SUCCESS) {
            task_res = (oss_part_task_result_t*)task_result;
            oss_update_checkpoint(parent_pool, checkpoint, task_res->part->index, &task_res->etag);
            rv = oss_dump_checkpoint(parent_pool, checkpoint);
            if (rv != AOSE_OK) {
                int idx = task_res->part->index;
                aos_status_set(ret, rv, AOS_WRITE_FILE_ERROR_CODE, NULL);
                apr_atomic_inc32(&failed);
                thr_params[idx].result->s = ret;
                apr_queue_push(failed_parts, thr_params[idx].result);
            }
            if (NULL != progress_callback) {
                consume_bytes += task_res->part->size;
                progress_callback(consume_bytes, finfo->size);
            }
        }
        total_num = apr_atomic_read32(&launched) + apr_atomic_read32(&failed) + apr_atomic_read32(&completed);
    }

    // deal with left successful parts
    while(APR_SUCCESS == apr_queue_trypop(completed_parts, &task_result)) {
        task_res = (oss_part_task_result_t*)task_result;
        oss_update_checkpoint(parent_pool, checkpoint, task_res->part->index, &task_res->etag);
        consume_bytes += task_res->part->size;
        has_left_result = AOS_TRUE;
    }
    if (has_left_result) {
        rv = oss_dump_checkpoint(parent_pool, checkpoint);
        if (rv != AOSE_OK) {
            aos_status_set(ret, rv, AOS_WRITE_FILE_ERROR_CODE, NULL);
            return ret;
        }
        if (NULL != progress_callback) {
            progress_callback(consume_bytes, finfo->size);
        }
    }
    apr_file_close(checkpoint->thefile);

    // failed
    if (apr_atomic_read32(&failed) > 0) {
        apr_queue_pop(failed_parts, &task_result);
        task_res = (oss_part_task_result_t*)task_result;
        s = aos_status_dup(parent_pool, task_res->s);
        oss_destroy_thread_pool(thr_params, part_num);
        return s;
    }
    
    // successful
    aos_pool_create(&subpool, parent_pool);
    aos_list_init(&completed_part_list);
    for (i = 0; i < checkpoint->part_num; i++) {
        complete_content = oss_create_complete_part_content(subpool);
        part_num_str = apr_psprintf(subpool, "%d", checkpoint->parts[i].index + 1);
        aos_str_set(&complete_content->part_number, part_num_str);
        aos_str_set(&complete_content->etag, checkpoint->parts[i].etag.data);
        aos_list_add_tail(&complete_content->node, &completed_part_list);
    }
    oss_destroy_thread_pool(thr_params, part_num);

    // complete upload
    options->pool = subpool;
    if (NULL != headers && NULL != apr_table_get(headers, OSS_CALLBACK)) {
        cb_headers = aos_table_make(subpool, 2);
        apr_table_set(cb_headers, OSS_CALLBACK, apr_table_get(headers, OSS_CALLBACK));
        if (NULL != apr_table_get(headers, OSS_CALLBACK_VAR)) {
            apr_table_set(cb_headers, OSS_CALLBACK_VAR, apr_table_get(headers, OSS_CALLBACK_VAR));
        }
    }
    s = oss_do_complete_multipart_upload(options, bucket, object, &upload_id, 
        &completed_part_list, cb_headers, NULL, resp_headers, resp_body);
    s = aos_status_dup(parent_pool, s);
    aos_pool_destroy(subpool);
    options->pool = parent_pool;

    // remove chepoint file
    apr_file_remove(checkpoint_path->data, parent_pool);
    
    return s;
}

aos_status_t *oss_resumable_upload_file(oss_request_options_t *options,
                                        aos_string_t *bucket, 
                                        aos_string_t *object, 
                                        aos_string_t *filepath,                           
                                        aos_table_t *headers,
                                        aos_table_t *params,
                                        oss_resumable_clt_params_t *clt_params, 
                                        oss_progress_callback progress_callback,
                                        aos_table_t **resp_headers,
                                        aos_list_t *resp_body) 
{
    int32_t thread_num = 0;
    int64_t part_size = 0;
    aos_string_t checkpoint_path;
    aos_pool_t *sub_pool;
    apr_finfo_t finfo;
    aos_status_t *s;
    int res;

    thread_num = oss_get_thread_num(clt_params);

    aos_pool_create(&sub_pool, options->pool);
    res = oss_get_file_info(filepath, sub_pool, &finfo);
    if (res != AOSE_OK) {
        aos_error_log("Open read file fail, filename:%s\n", filepath->data);
        s = aos_status_create(options->pool);
        aos_file_error_status_set(s, res);
        aos_pool_destroy(sub_pool);
        return s;
    }
    part_size = clt_params->part_size;
    oss_get_part_size(finfo.size, &part_size);

    if (NULL != clt_params && clt_params->enable_checkpoint) {
        oss_get_checkpoint_path(clt_params, filepath, sub_pool, &checkpoint_path);
        s = oss_resumable_upload_file_with_cp(options, bucket, object, filepath, headers, params, thread_num, 
            part_size, &checkpoint_path, &finfo, progress_callback, resp_headers, resp_body);
    } else {
        s = oss_resumable_upload_file_without_cp(options, bucket, object, filepath, headers, params, thread_num, 
            part_size, &finfo, progress_callback, resp_headers, resp_body);
    }

    aos_pool_destroy(sub_pool);
    return s;
}
