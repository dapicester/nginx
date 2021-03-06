extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include <tfs_client_api.h>
#include <func.h>
#include <Magick++.h>
#include <tblog.h>

using namespace std;
using namespace tfs::client;
using namespace tfs::common;
using namespace Magick;

#define DEFAULT_TFS_READ_WRITE_SIZE (2 * 1024 * 1024)
#define ZOOMPARAM_LEN 64
#define QUALITY_LEN 10
#define WATERMARK_LEN 6
#define scast(T,V)                      static_cast< T >( (V) )
#define TFS_NS_ARRAY_INIT_SIZE             8

static void* ngx_http_tfs_create_main_conf(ngx_conf_t *cf);
static void* ngx_http_tfs_create_loc_conf(ngx_conf_t *cf);
static char* ngx_http_tfs_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char* ngx_http_tfs_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_tfs_init_process(ngx_cycle_t* cycle);
static void ngx_tfs_exit_process(ngx_cycle_t* cycle);
static bool get_arg_value(ngx_http_request_t *r, const u_char* args_str, int find_offset, const ngx_str_t* arg_key, int maxlen, u_char* arg_value);

typedef struct {
    ngx_str_t tfs_nsip; 		
    size_t tfs_rb_buffer_size;
    ngx_str_t watermark_file;
    TfsClient* tfsclient;
} ngx_http_tfs_ns_loc_conf_t;

typedef struct {
    ngx_array_t loc_confs; // array of ngx_http_tfs_ns_loc_conf_t*
} ngx_http_tfs_ns_main_conf_t;

static ngx_command_t  ngx_http_tfs_commands[] = {
    { ngx_string("tfs_get"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, /* 不带参数 */
      ngx_http_tfs_get,
      0,
      0,
      NULL },

    { ngx_string("tfs_nsip"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot	,            	/* 直接调用内置的字符串解释函数解释参数*/
      NGX_HTTP_LOC_CONF_OFFSET,         	/* 只在location中配置 */
      offsetof(ngx_http_tfs_ns_loc_conf_t, tfs_nsip),
      NULL },

    { ngx_string("tfs_rb_buffer_size"),		/* 每次读写tfs文件buffer大小  */
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tfs_ns_loc_conf_t, tfs_rb_buffer_size),
      NULL },

    { ngx_string("watermark_file"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot	,            	/* 直接调用内置的字符串解释函数解释参数*/
      NGX_HTTP_LOC_CONF_OFFSET,         	/* 只在location中配置 */
      offsetof(ngx_http_tfs_ns_loc_conf_t, watermark_file),
      NULL },


      ngx_null_command
};

static ngx_http_module_t  ngx_http_tfs_module_ctx = {
    NULL,                          /* preconfiguration */
    NULL,                          /* postconfiguration */

    ngx_http_tfs_create_main_conf, /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_tfs_create_loc_conf,  /* create location configuration  创建location配置时调用 */
    ngx_http_tfs_merge_loc_conf    /* merge location configuration  与location配置合并时调用 */
};

ngx_module_t  ngx_http_tfs_module = {
    NGX_MODULE_V1,
    &ngx_http_tfs_module_ctx, /* module context */
    ngx_http_tfs_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    ngx_tfs_init_process,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_tfs_exit_process,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t 
ngx_tfs_init_process(ngx_cycle_t* cycle) {

    InitializeMagick(NULL);   

    ngx_http_tfs_ns_main_conf_t* main_conf = scast(ngx_http_tfs_ns_main_conf_t*, ngx_http_cycle_get_module_main_conf(cycle, ngx_http_tfs_module));
    ngx_http_tfs_ns_loc_conf_t** loc_confs = scast(ngx_http_tfs_ns_loc_conf_t**,
        main_conf->loc_confs.elts);

    ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "init tfs handle process, tfs conf num:%d", main_conf->loc_confs.nelts);
    for( ngx_uint_t i = 0; i < main_conf->loc_confs.nelts; i++ )
    {
        ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "tfs conf init for:%s.", loc_confs[i]->tfs_nsip.data);
        if(loc_confs[i] == NULL)
        {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "location conf is null.");
            return NGX_ERROR;
        }
        loc_confs[i]->tfsclient = TfsClient::Instance();
        int ret = loc_confs[i]->tfsclient->initialize((const char*)loc_confs[i]->tfs_nsip.data);
        if(ret != TFS_SUCCESS)
        {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "init tfsclient failed.");
            loc_confs[i]->tfsclient = NULL;
            continue;
        }
    }
    TBSYS_LOGGER.setLogLevel("WARN");
    return NGX_OK;
}

static void
ngx_tfs_exit_process(ngx_cycle_t* cycle) {
    ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "exit tfs handle process");
}

static bool get_arg_value(ngx_http_request_t *r, const u_char* args_str, int find_offset, const ngx_str_t* arg_key, int maxlen, u_char* arg_value)
{
    arg_value[0] = '\0';
    const char* key_pos = ngx_strstr(args_str + find_offset, arg_key->data);
    if(key_pos != NULL)
    {
        const char* separate_char = ngx_strchr( key_pos + arg_key->len, '&');
        int key_param_len = r->args.len - (key_pos - (const char*)args_str + arg_key->len) + 1;
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "find key_pos:%s, %zu", key_pos, r->args.len);
        if(separate_char != NULL)
        {
            key_param_len = separate_char - ((const char*)key_pos + arg_key->len) + 1;
        }
        if(key_param_len > maxlen)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "key %s param too long:%d.", arg_key->data, key_param_len);
            return false;
        }
        u_char* tmp = (u_char*)ngx_create_temp_buf(r->pool, key_param_len);
        strncpy((char*)tmp, key_pos + arg_key->len, key_param_len);
        tmp[key_param_len - 1] = '\0';
        u_char* p = arg_value;
        ngx_unescape_uri(&p, &tmp, key_param_len, NGX_UNESCAPE_URI);
        arg_value[key_param_len - 1] = '\0';
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "arg_key %s, value:%s.", arg_key->data, arg_value);
        return true;
    }
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "arg_key %s not found.", arg_key->data);
    return false;
 }

static ngx_int_t
ngx_http_tfs_get_args_tfsname(ngx_http_request_t *r, u_char *ret, u_char* zoomparam, u_char* qualityparam, u_char* watermarkparam)
{
	static const ngx_str_t TFSNAME_KEY = ngx_string("filename=");
	static const ngx_str_t ZOOMPARAM_KEY = ngx_string("zoom=");
	static const ngx_str_t QUALITY_KEY = ngx_string("quality=");
	static const ngx_str_t WATERMARK_KEY = ngx_string("watermark=");
    static const int32_t MAX_ARGS_LEN = TFSNAME_KEY.len + ZOOMPARAM_KEY.len + QUALITY_KEY.len + WATERMARK_KEY.len + TFS_FILE_LEN + 1 + ZOOMPARAM_LEN + QUALITY_LEN + WATERMARK_LEN + 1;
    u_char args_str[MAX_ARGS_LEN];
    ngx_cpystrn(args_str, r->args.data, min(MAX_ARGS_LEN - 1, (int32_t)r->args.len + 1));
    args_str[MAX_ARGS_LEN - 1] = '\0';

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get args : %s.", args_str);
	if(r->args.len < TFSNAME_KEY.len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "tfs get args failed 1.%s, %zu.", r->args.data, r->args.len);
		return NGX_ERROR;
	}

	if(ngx_strncasecmp(args_str, TFSNAME_KEY.data, TFSNAME_KEY.len) == 0) {

        const char* separate_char = ngx_strchr( args_str + TFSNAME_KEY.len, '&');
        int tfs_filename_len = TFS_FILE_LEN;
        if(separate_char != NULL)
        {
            tfs_filename_len = separate_char - ((const char*)args_str + TFSNAME_KEY.len) + 1;
            if( tfs_filename_len > TFS_FILE_LEN)
            {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "tfs filename length is invalid:%d.", tfs_filename_len);
                return NGX_ERROR;
            }
        }
		ngx_cpystrn(ret, (args_str + TFSNAME_KEY.len), tfs_filename_len);
        ret[tfs_filename_len - 1] = '\0';
        bool get_arg_ret = get_arg_value(r, args_str, TFSNAME_KEY.len + tfs_filename_len, &ZOOMPARAM_KEY, ZOOMPARAM_LEN, zoomparam);
        if (get_arg_ret)
        {
            get_arg_ret = get_arg_value(r, args_str, TFSNAME_KEY.len + tfs_filename_len, &QUALITY_KEY, QUALITY_LEN, qualityparam);
            if(get_arg_ret)
            {
                ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get file with quality: %s.",  qualityparam);
            }
            else
            {
                ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get file zoom with default quality.");
            }
        }
        else
        {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get file without zoom.");
        }
        get_arg_ret = get_arg_value(r, args_str, TFSNAME_KEY.len + tfs_filename_len, &WATERMARK_KEY, WATERMARK_LEN, watermarkparam);
        if (get_arg_ret)
        {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get file with watermark.");
        }
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs get filename:%s, zoom param:%s, watermarkparam:%s", ret, zoomparam, watermarkparam);
		return NGX_OK;
	}
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "tfs get args failed 2. %s, %zu, %zu.", args_str, r->args.len, TFSNAME_KEY.len);
	return NGX_ERROR;
}

static ngx_int_t
ngx_http_tfs_get_handler(ngx_http_request_t *r)
{
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "got tfs get request.");
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t   out;
    u_char tfsname[TFS_FILE_LEN + 1];
    u_char zoomparam[ZOOMPARAM_LEN + 1] = {'\0'};
    u_char qualityparam[QUALITY_LEN + 1] = {'\0'};
    u_char watermarkparam[WATERMARK_LEN + 1] = {'\0'};
    ngx_http_tfs_ns_loc_conf_t  *cglcf;

    cglcf = (ngx_http_tfs_ns_loc_conf_t*)ngx_http_get_module_loc_conf(r, ngx_http_tfs_module);

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "reject tfs get request for not allowed method : %u.", r->method);
        return NGX_HTTP_NOT_ALLOWED;
    }
    if (r->headers_in.if_modified_since) {
        return NGX_HTTP_NOT_MODIFIED;
    }

    if( NGX_OK != ngx_http_tfs_get_args_tfsname(r, tfsname, zoomparam, qualityparam, watermarkparam)) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "reject tfs get request for wrong args. ");
    	return NGX_HTTP_NOT_ALLOWED;
    }
    int ret = 0;
    int fd = -1;
    if(cglcf->tfsclient == NULL)
    {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "tfs ns: %s", cglcf->tfs_nsip.data);
        cglcf->tfsclient = TfsClient::Instance();
        int ret = cglcf->tfsclient->initialize((const char*)cglcf->tfs_nsip.data);
        if(ret != TFS_SUCCESS)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "reinit TFS client failed.");
            cglcf->tfsclient = NULL;
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0 , "TFS client reinit success.");
    }

	TfsClient* tfsclient = cglcf->tfsclient;
	fd = tfsclient->open((const char*)tfsname, NULL, T_READ);
    if(fd <= 0)
    {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "TFS open file failed.");
        return NGX_HTTP_NOT_FOUND;
    }
	TfsFileStat finfo;
	ret = tfsclient->fstat(fd, &finfo);
	if (ret != TFS_SUCCESS || finfo.size_ <= 0)	{
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "TFS fstat file failed.");
        return NGX_HTTP_NOT_FOUND;
	}

	b = (ngx_buf_t *)ngx_create_temp_buf(r->pool, finfo.size_);
    if (b == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response buffer: %u.\n", finfo.size_);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last = b->pos + finfo.size_ ;
    b->memory = 1;
    b->last_buf = 1;

	int32_t read = 0;
 	int32_t read_size;
	uint32_t crc = 0;
	size_t left = finfo.size_;
	while (read < finfo.size_) {
		read_size = left > cglcf->tfs_rb_buffer_size ? cglcf->tfs_rb_buffer_size : left;
		ret = tfsclient->read(fd, (char*)b->pos + read, read_size);
		if (ret < 0) {
			break;
		}
		else {
			crc = Func::crc(crc, (const char*)(b->pos + read), ret); 
			read += ret;
			left -= ret;
		}
	}

	if (ret < 0 || crc != finfo.crc_) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "TFS read file failed.");
        return NGX_HTTP_NOT_FOUND;
	}

	ret = tfsclient->close(fd);
	if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "TFS close file failed.");
        return NGX_HTTP_NOT_FOUND;
	}

    //static const ngx_str_t good_str = ngx_string("good");
    if(ngx_strlen(zoomparam) > 0 || ngx_strlen(watermarkparam) > 0)
    {
        Blob imgdata;
        imgdata.update((char*)b->pos, finfo.size_);

        Image image;
        Blob zoomed_imgdata;
        bool resize_error = false;
        try
        {
            image.read(imgdata);

            if (ngx_strlen(watermarkparam) > 0 && cglcf->watermark_file.len > 0)
            {
                ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "watermark file:%s", cglcf->watermark_file.data);
                // read watermark image.
                Image watermark;
                watermark.read((const char*)cglcf->watermark_file.data);
                // 
                //#define ForgetGravity		0
                //#define NorthWestGravity	1
                //#define NorthGravity		2
                //#define NorthEastGravity	3
                //#define WestGravity		4
                //#define CenterGravity		5
                //#define EastGravity		6
                //#define SouthWestGravity	7
                //#define SouthGravity		8
                //#define SouthEastGravity	9
                //#define StaticGravity		10

                ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "watermark size :%d, %d", watermark.columns(), watermark.rows());
                image.composite(watermark, SouthEastGravity, OverCompositeOp);
            }

            Geometry zoom_param_geo((const char*)zoomparam);
            if(zoom_param_geo <= image.size())
            {
                if(zoom_param_geo.width() == 0)
                    zoom_param_geo.width(image.columns());
                if(zoom_param_geo.height() == 0)
                    zoom_param_geo.height(image.rows());
                if(ngx_strlen(qualityparam) > 0)
                {
                    unsigned int quality_i = 75;
                    sscanf((const char*)qualityparam, "%d", &quality_i);
                    if(quality_i < 50)
                        quality_i = 50;
                    if(quality_i > 98)
                        quality_i = 98;
                    image.zoom(zoom_param_geo);
                    image.quality(quality_i);
                }
                else
                {
                    image.scale(zoom_param_geo);
                }
            }
            image.write(&zoomed_imgdata);
        }
        catch( Magick::Exception &e)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Image resize throw magick exception. msg: %s", e.what());
            resize_error = true;
            //return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        catch( ... )
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Image resize caught unknown exception.");
            resize_error = true;
            //return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if(zoomed_imgdata.data() == NULL)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Image resize data is null.");
            resize_error = true;
            //return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if(resize_error || 
            (ngx_strlen(zoomparam) > 0 && ((int)zoomed_imgdata.length() > (int)finfo.size_)) )
        {
            if(!resize_error)
            {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Image resize data is larger than original data which is not allowed, so just return the original data.");
            }
            r->headers_out.content_length_n = finfo.size_;
        }
        else
        {
            if (zoomed_imgdata.length() > finfo.size_)
            {
                b = (ngx_buf_t *)ngx_create_temp_buf(r->pool, zoomed_imgdata.length());
                if (b == NULL) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response buffer: %u.\n", finfo.size_);
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                b->last = b->pos + zoomed_imgdata.length() ;
                b->memory = 1;
                b->last_buf = 1;
            }

            ngx_copy(b->pos, zoomed_imgdata.data(), zoomed_imgdata.length());
            r->headers_out.content_length_n = zoomed_imgdata.length();
            b->last = b->pos + zoomed_imgdata.length();
        }
    }
    else
    {
        r->headers_out.content_length_n = finfo.size_;
    }

    out.buf = b;
    out.next = NULL;


    r->headers_out.content_type.len = sizeof("image/jpeg") - 1;
    r->headers_out.content_type.data = (u_char *) "image/jpeg";
    r->headers_out.status = NGX_HTTP_OK;

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "Image resize process finished. content-len: %d, type:%s.", r->headers_out.content_length_n, r->headers_out.content_type.data);

    if (r->method == NGX_HTTP_HEAD) {
        rc = ngx_http_send_header(r);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static char *
ngx_http_tfs_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = reinterpret_cast<ngx_http_core_loc_conf_t*>(
    			ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module));
    clcf->handler = ngx_http_tfs_get_handler;
    return NGX_CONF_OK;
}

static void *
ngx_http_tfs_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_tfs_ns_main_conf_t* conf = scast(ngx_http_tfs_ns_main_conf_t*, 
            ngx_pcalloc(cf->pool, sizeof(ngx_http_tfs_ns_main_conf_t)));
    if (conf == NULL) {
        ngx_log_error(NGX_LOG_ERR, cf->log, 0, "failed to allocate memory");
        return NULL;
    }
    
    if (ngx_array_init(&conf->loc_confs, cf->pool, TFS_NS_ARRAY_INIT_SIZE,
            sizeof(ngx_http_tfs_ns_loc_conf_t*)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cf->log, 0, "failed to allocate memory");
        return NGX_CONF_ERROR;
    }
    
    return conf;
}

static void *
ngx_http_tfs_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_tfs_ns_loc_conf_t  *conf;

    conf = (ngx_http_tfs_ns_loc_conf_t *)ngx_pcalloc(cf->pool, sizeof(ngx_http_tfs_ns_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->tfs_rb_buffer_size = (size_t)DEFAULT_TFS_READ_WRITE_SIZE;
    conf->tfsclient = NULL;

    return conf;
}

static char *
ngx_http_tfs_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_tfs_ns_loc_conf_t *prev = (ngx_http_tfs_ns_loc_conf_t *)parent;
    ngx_http_tfs_ns_loc_conf_t *conf = (ngx_http_tfs_ns_loc_conf_t *)child;

    ngx_conf_merge_str_value(conf->tfs_nsip, prev->tfs_nsip, "127.0.0.1:8108");
    ngx_conf_merge_size_value(conf->tfs_rb_buffer_size, prev->tfs_rb_buffer_size, (size_t)DEFAULT_TFS_READ_WRITE_SIZE);

    ngx_conf_merge_str_value(conf->watermark_file, prev->watermark_file, "");
    ngx_conf_merge_ptr_value(conf->tfsclient, prev->tfsclient, NULL);

    // add to the main conf struct
    ngx_http_tfs_ns_main_conf_t* main = scast(ngx_http_tfs_ns_main_conf_t*, 
            ngx_http_conf_get_module_main_conf(cf, ngx_http_tfs_module));
    ngx_http_tfs_ns_loc_conf_t** loc = scast(ngx_http_tfs_ns_loc_conf_t**, ngx_array_push(&main->loc_confs));
    *loc = conf;
    return NGX_CONF_OK;
}

