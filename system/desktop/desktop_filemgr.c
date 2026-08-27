/****************************************************************************
 * apps/system/desktop/desktop_filemgr.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "desktop_filemgr.h"

#define FM_HEADER_SIZE       4096
#define FM_IO_SIZE           1024
#define FM_TARGET_SIZE       512
#define FM_PATH_SIZE         256
#define FM_TOKEN_SIZE        7
#define FM_WORKER_STACK      8192
#define FM_MAX_UPLOAD        (8 * 1024 * 1024)
#define FM_UPLOAD_TIMEOUT    60
#define FM_RESERVE_BLOCKS    2
#define FM_STRINGIFY_INNER(value) #value
#define FM_STRINGIFY(value) FM_STRINGIFY_INNER(value)

struct fm_request_s
{
  char method[8];
  char target[FM_TARGET_SIZE];
  size_t content_length;
  size_t body_offset;
  bool has_content_length;
  bool expect_continue;
};

struct fm_server_s
{
  pthread_mutex_t lock;
  int listen_fd;
  bool running;
  char token[FM_TOKEN_SIZE];
};

struct fm_stream_s
{
  int fd;
  int status;
  size_t used;
  char buffer[FM_IO_SIZE];
};

static struct fm_server_s g_fm =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .listen_fd = -1,
};

static const char g_fm_html[] =
  "<!doctype html><html lang=\"zh-CN\"><head>"
  "<meta charset=\"utf-8\"><meta name=\"viewport\" "
  "content=\"width=device-width,initial-scale=1\">"
  "<title>NuttX 文件管理</title><style>"
  ":root{color-scheme:light dark;--bg:#f4f6f9;--panel:#fff;--text:#172033;"
  "--muted:#687386;--line:#d9dee7;--blue:#3659c9;--danger:#c83d4b}"
  "@media(prefers-color-scheme:dark){:root{--bg:#101521;--panel:#171e2c;"
  "--text:#f4f6fb;--muted:#9ba6ba;--line:#303a4d;--blue:#7890ee;"
  "--danger:#ef7180}}*{box-sizing:border-box}body{margin:0;background:var(--bg);"
  "color:var(--text);font:15px system-ui,sans-serif}header{height:58px;"
  "display:flex;align-items:center;justify-content:space-between;padding:0 24px;"
  "background:var(--panel);border-bottom:1px solid var(--line)}h1{font-size:19px;"
  "margin:0;letter-spacing:0}.status{color:var(--muted);font-size:13px}"
  "main{max-width:980px;margin:22px auto;padding:0 18px}.toolbar{display:flex;"
  "gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}.path{flex:1;"
  "min-width:220px;padding:9px 11px;background:var(--panel);border:1px solid "
  "var(--line);border-radius:6px;overflow:hidden;text-overflow:ellipsis}"
  "button,.upload{border:1px solid var(--line);background:var(--panel);"
  "color:var(--text);padding:9px 13px;border-radius:6px;cursor:pointer;"
  "font:inherit}button:hover,.upload:hover{border-color:var(--blue)}"
  ".primary{background:var(--blue);border-color:var(--blue);color:#fff}"
  ".upload input{display:none}.table{background:var(--panel);border:1px solid "
  "var(--line);border-radius:8px;overflow:hidden}.row{display:grid;"
  "grid-template-columns:minmax(180px,1fr) 100px 160px;align-items:center;"
  "min-height:52px;padding:0 14px;border-bottom:1px solid var(--line);gap:12px}"
  ".row:last-child{border-bottom:0}.name{display:flex;gap:10px;align-items:center;"
  "min-width:0}.name button{border:0;padding:4px;background:transparent;"
  "color:var(--text);overflow:hidden;text-overflow:ellipsis;text-align:left;"
  "white-space:nowrap}.kind{width:28px;color:var(--muted);font-size:12px}"
  ".size{color:var(--muted);font-variant-numeric:tabular-nums}.actions{display:flex;"
  "justify-content:flex-end;gap:6px}.actions button{padding:6px 9px;font-size:13px}"
  ".danger{color:var(--danger)}.empty{padding:48px;text-align:center;"
  "color:var(--muted)}.message{min-height:24px;margin-top:12px;color:var(--muted)}"
  ".progress{display:block;width:100%;height:8px;accent-color:var(--blue)}"
  ".progress[hidden]{display:none}"
  "@media(max-width:620px){header{padding:0 16px}.row{grid-template-columns:1fr "
  "auto}.size{display:none}.actions{grid-column:1/-1;justify-content:flex-start;"
  "padding-bottom:10px}}</style></head><body>"
  "<header><h1>NuttX 文件管理</h1><span class=\"status\">/data</span></header>"
  "<main><div class=\"toolbar\"><button id=\"up\">上一级</button>"
  "<div class=\"path\" id=\"path\">/</div><button id=\"mkdir\">新建目录</button>"
  "<label class=\"upload primary\">上传文件<input id=\"files\" type=\"file\" "
  "multiple></label></div><div class=\"table\" id=\"list\"></div>"
  "<div class=\"message\" id=\"message\"></div>"
  "<progress class=\"progress\" id=\"progress\" max=\"100\" value=\"0\" "
  "hidden></progress></main><script>"
  "const qs=new URLSearchParams(location.search);let key=qs.get('key')||"
  "sessionStorage.getItem('fmkey')||'';if(!key){key=prompt('请输入设备上显示的访问码')"
  "||''}if(key)sessionStorage.setItem('fmkey',key);let path='/',freeBytes=null,"
  "knownFiles=new Map();"
  "const list=document.getElementById('list'),msg=document.getElementById('message'),"
  "pathEl=document.getElementById('path'),progress=document.getElementById('progress'),"
  "maxName=" FM_STRINGIFY(CONFIG_NAME_MAX) ";"
  "function api(route,p){const q=new "
  "URLSearchParams(p||{});q.set('key',key);return route+'?'+q}"
  "function join(a,b){return(a==='/'?'':a)+'/'+b}function size(n){if(n<1024)"
  "return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return"
  "(n/1048576).toFixed(1)+' MB'}function el(tag,cls,text){const n=document."
  "createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;"
  "return n}async function request(url,opt){const r=await fetch(url,opt);if(!r.ok){let t="
  "await r.text();throw new Error(t||('HTTP '+r.status))}return r}"
  "function message(t,bad){msg.textContent=t;msg.style.color=bad?'var(--danger)':''}"
  "async function refresh(){message('正在读取目录');try{const r=await request(api"
  "('/api/list',{path}));const data=await r.json();pathEl.textContent=data.path;"
  "freeBytes=Number.isFinite(data.free)?data.free:null;knownFiles=new Map(data.items."
  "filter(x=>!x.dir).map(x=>[x.name,x.size]));"
  "list.replaceChildren();if(!data.items.length)list.append(el('div','empty',"
  "'此目录为空'));for(const item of data.items){const row=el('div','row');const "
  "name=el('div','name');name.append(el('span','kind',item.dir?'目录':'文件'));"
  "const open=el('button','',item.name);open.onclick=()=>item.dir?openDir(item.name):"
  "download(item.name);name.append(open);row.append(name,el('div','size',item.dir?"
  "'--':size(item.size)));const actions=el('div','actions');if(!item.dir){const d="
  "el('button','','下载');d.onclick=()=>download(item.name);actions.append(d)}const "
  "del=el('button','danger','删除');del.onclick=()=>removeItem(item);actions.append"
  "(del);row.append(actions);list.append(row)}message(data.items.length+' 个项目'+"
  "(freeBytes===null?'':' · 可用 '+size(freeBytes)))}"
  "catch(e){list.replaceChildren();list.append(el('div','empty','无法读取目录'));"
  "message(e.message,true)}}function openDir(n){path=join(path,n);refresh()}"
  "function download(n){const a=document.createElement('a');a.href=api('/api/download',"
  "{path:join(path,n)});a.download=n;a.click()}async function removeItem(item){if(!confirm"
  "('确定删除 '+item.name+'？'))return;try{await request(api('/api/delete',{path:join"
  "(path,item.name)}),{method:'DELETE'});await refresh()}catch(e){message(e.message,true)}}"
  "document.getElementById('up').onclick=()=>{if(path==='/')return;const i=path."
  "lastIndexOf('/');path=i<=0?'/':path.slice(0,i);refresh()};document.getElementById"
  "('mkdir').onclick=async()=>{const name=prompt('目录名称');if(!name)return;try{await "
  "request(api('/api/mkdir',{path:join(path,name)}),{method:'POST'});await refresh()}"
  "catch(e){message(e.message,true)}};function upload(file){return new Promise((ok,fail)=>{"
  "const x=new XMLHttpRequest();x.open('PUT',api('/api/upload',{path:join(path,file.name)}));"
  "x.upload.onprogress=e=>{if(e.lengthComputable){const n=Math.round(e.loaded*100/e.total);"
  "progress.hidden=false;progress.value=n;message('正在上传 '+file.name+' · '+n+'%')}};"
  "x.onload=()=>x.status>=200&&x.status<300?ok():fail(new Error(x.responseText||"
  "('HTTP '+x.status)));x.onerror=()=>fail(new Error('上传连接中断'));x.onabort=()=>"
  "fail(new Error('上传已取消'));x.send(file)})}document.getElementById('files').onchange="
  "async e=>{for(const file of e.target.files){const n=new Blob([file.name]).size;if(n>"
  "maxName){e.target.value='';progress.hidden=true;message('文件名过长：'+n+' 字节，设备"
  "最多支持 '+maxName+' 字节（中文通常每字 3 字节）',true);return}}if(freeBytes!=="
  "null){let budget=freeBytes;for(const file of e.target.files){const old=knownFiles.get("
  "file.name)||0;if(file.size>budget+old){e.target.value='';progress.hidden=true;message("
  "'存储空间不足：'+file.name+' 需要 '+size(file.size)+'，当前可用 '+size(budget+old),"
  "true);return}budget+=old-file.size}}progress.value=0;"
  "progress.hidden=false;for(const file of e.target.files){try{"
  "await upload(file)}catch(err){progress.hidden=true;message(err.message,true);return}}"
  "e.target.value='';progress.hidden=true;await refresh()};"
  "refresh();</script></body></html>";

static int fm_send_all(int fd, const void *buffer, size_t length)
{
  const uint8_t *cursor = buffer;

  while (length > 0)
    {
      ssize_t sent = send(fd, cursor, length, 0);

      if (sent < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (sent == 0)
        {
          return -EPIPE;
        }

      cursor += sent;
      length -= sent;
    }

  return 0;
}

static int fm_write_all(int fd, const void *buffer, size_t length)
{
  const uint8_t *cursor = buffer;

  while (length > 0)
    {
      ssize_t written = write(fd, cursor, length);

      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (written == 0)
        {
          return -ENOSPC;
        }

      cursor += written;
      length -= written;
    }

  return 0;
}

static int fm_send_text(int fd, const char *text)
{
  return fm_send_all(fd, text, strlen(text));
}

static int fm_storage_space(uint64_t *total, uint64_t *available)
{
  struct statvfs info;
  uint64_t block_size;
  uint64_t free_bytes;
  uint64_t reserve;

  if (statvfs(CONFIG_SYSTEM_DESKTOP_FILEMGR_ROOT, &info) < 0)
    {
      return -errno;
    }

  block_size = info.f_frsize != 0 ? info.f_frsize : info.f_bsize;
  free_bytes = (uint64_t)info.f_bavail * block_size;
  reserve = block_size * FM_RESERVE_BLOCKS;
  if (total != NULL)
    {
      *total = (uint64_t)info.f_blocks * block_size;
    }

  *available = free_bytes > reserve ? free_bytes - reserve : 0;
  return 0;
}

static int fm_stream_flush(struct fm_stream_s *stream)
{
  if (stream->status < 0 || stream->used == 0)
    {
      return stream->status;
    }

  stream->status = fm_send_all(stream->fd, stream->buffer, stream->used);
  stream->used = 0;
  return stream->status;
}

static int fm_stream_write(struct fm_stream_s *stream,
                           const void *buffer, size_t length)
{
  const char *cursor = buffer;

  while (stream->status == 0 && length > 0)
    {
      size_t space = sizeof(stream->buffer) - stream->used;
      size_t chunk;

      if (space == 0)
        {
          fm_stream_flush(stream);
          continue;
        }

      chunk = length < space ? length : space;
      memcpy(stream->buffer + stream->used, cursor, chunk);
      stream->used += chunk;
      cursor += chunk;
      length -= chunk;
    }

  return stream->status;
}

static int fm_stream_text(struct fm_stream_s *stream, const char *text)
{
  return fm_stream_write(stream, text, strlen(text));
}

static int fm_stream_json_string(struct fm_stream_s *stream,
                                 const char *text)
{
  const unsigned char *cursor = (const unsigned char *)text;

  fm_stream_text(stream, "\"");
  while (stream->status == 0 && *cursor != '\0')
    {
      char escaped[7];

      if (*cursor == '"' || *cursor == '\\')
        {
          escaped[0] = '\\';
          escaped[1] = *cursor++;
          fm_stream_write(stream, escaped, 2);
        }
      else if (*cursor < 0x20)
        {
          snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor++);
          fm_stream_write(stream, escaped, 6);
        }
      else
        {
          fm_stream_write(stream, cursor++, 1);
        }
    }

  return fm_stream_text(stream, "\"");
}

static int fm_send_header(int fd, int status, const char *reason,
                          const char *type, ssize_t length,
                          const char *extra)
{
  char header[384];
  int size;

  if (length >= 0)
    {
      size = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                      "Content-Length: %ld\r\nConnection: close\r\n%s\r\n",
                      status, reason, type, (long)length,
                      extra != NULL ? extra : "");
    }
  else
    {
      size = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                      "Connection: close\r\n%s\r\n",
                      status, reason, type, extra != NULL ? extra : "");
    }

  if (size < 0 || (size_t)size >= sizeof(header))
    {
      return -EOVERFLOW;
    }

  return fm_send_all(fd, header, size);
}

static void fm_send_error(int fd, int status, const char *reason,
                          const char *message)
{
  char body[192];
  int size;

  size = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
  if (size < 0)
    {
      return;
    }

  if ((size_t)size >= sizeof(body))
    {
      size = sizeof(body) - 1;
    }

  if (fm_send_header(fd, status, reason, "application/json; charset=utf-8",
                     size, "Cache-Control: no-store\r\n") == 0)
    {
      fm_send_all(fd, body, size);
    }
}

static ssize_t fm_header_end(const char *buffer, size_t length)
{
  size_t i;

  for (i = 3; i < length; i++)
    {
      if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' &&
          buffer[i - 1] == '\r' && buffer[i] == '\n')
        {
          return i + 1;
        }
    }

  return -1;
}

static int fm_read_request(int fd, char *buffer, size_t buffer_size,
                           size_t *received, struct fm_request_s *request)
{
  char version[16];
  char *line;
  char *next;
  ssize_t end;

  *received = 0;
  memset(request, 0, sizeof(*request));
  while (*received + 1 < buffer_size)
    {
      ssize_t got = recv(fd, buffer + *received,
                         buffer_size - *received - 1, 0);

      if (got < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (got == 0)
        {
          return -ECONNRESET;
        }

      *received += got;
      end = fm_header_end(buffer, *received);
      if (end >= 0)
        {
          request->body_offset = end;
          break;
        }
    }

  if (request->body_offset == 0)
    {
      return -E2BIG;
    }

  buffer[*received] = '\0';
  line = buffer;
  next = strstr(line, "\r\n");
  if (next == NULL)
    {
      return -EINVAL;
    }

  *next = '\0';
  if (sscanf(line, "%7s %511s %15s", request->method,
             request->target, version) != 3 ||
      strncmp(version, "HTTP/", 5) != 0)
    {
      return -EINVAL;
    }

  line = next + 2;
  while (line < buffer + request->body_offset - 2)
    {
      next = strstr(line, "\r\n");
      if (next == NULL || next == line)
        {
          break;
        }

      *next = '\0';
      if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
          char *endptr;
          unsigned long value;

          errno = 0;
          value = strtoul(line + 15, &endptr, 10);
          while (isspace((unsigned char)*endptr))
            {
              endptr++;
            }

          if (errno != 0 || *endptr != '\0' || value > FM_MAX_UPLOAD)
            {
              return -EFBIG;
            }

          request->content_length = value;
          request->has_content_length = true;
        }
      else if (strncasecmp(line, "Expect:", 7) == 0 &&
               strstr(line + 7, "100-continue") != NULL)
        {
          request->expect_continue = true;
        }
      else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0)
        {
          return -ENOTSUP;
        }

      line = next + 2;
    }

  return 0;
}

static int fm_hex(char value)
{
  if (value >= '0' && value <= '9')
    {
      return value - '0';
    }

  if (value >= 'a' && value <= 'f')
    {
      return value - 'a' + 10;
    }

  if (value >= 'A' && value <= 'F')
    {
      return value - 'A' + 10;
    }

  return -1;
}

static int fm_query_value(const char *target, const char *name,
                          char *output, size_t output_size)
{
  const char *query = strchr(target, '?');
  size_t name_length = strlen(name);

  if (query == NULL)
    {
      return -ENOENT;
    }

  query++;
  while (*query != '\0')
    {
      const char *end = strchr(query, '&');
      const char *value;

      if (end == NULL)
        {
          end = query + strlen(query);
        }

      value = memchr(query, '=', end - query);
      if (value != NULL && (size_t)(value - query) == name_length &&
          strncmp(query, name, name_length) == 0)
        {
          const char *cursor = value + 1;
          size_t used = 0;

          while (cursor < end)
            {
              int decoded;

              if (*cursor == '%')
                {
                  int high;
                  int low;

                  if (end - cursor < 3 ||
                      (high = fm_hex(cursor[1])) < 0 ||
                      (low = fm_hex(cursor[2])) < 0)
                    {
                      return -EINVAL;
                    }

                  decoded = high * 16 + low;
                  cursor += 3;
                }
              else if (*cursor == '+')
                {
                  decoded = ' ';
                  cursor++;
                }
              else
                {
                  decoded = (unsigned char)*cursor++;
                }

              if (decoded == 0 || used + 1 >= output_size)
                {
                  return -ENAMETOOLONG;
                }

              output[used++] = decoded;
            }

          output[used] = '\0';
          return 0;
        }

      query = *end == '&' ? end + 1 : end;
    }

  return -ENOENT;
}

static int fm_data_path(const char *target, char *path, size_t path_size,
                        char *display, size_t display_size)
{
  char decoded[FM_PATH_SIZE];
  const char *cursor;
  size_t path_used;
  size_t display_used;
  int ret;

  ret = fm_query_value(target, "path", decoded, sizeof(decoded));
  if (ret < 0)
    {
      return ret;
    }

  cursor = decoded;
  while (*cursor == '/')
    {
      cursor++;
    }

  path_used = strlcpy(path, CONFIG_SYSTEM_DESKTOP_FILEMGR_ROOT, path_size);
  if (path_used >= path_size)
    {
      return -ENAMETOOLONG;
    }

  display_used = strlcpy(display, "/", display_size);
  if (display_used >= display_size)
    {
      return -ENAMETOOLONG;
    }

  while (*cursor != '\0')
    {
      const char *slash = strchr(cursor, '/');
      size_t segment_length = slash != NULL ? (size_t)(slash - cursor) :
                                              strlen(cursor);

      if (segment_length == 0)
        {
          cursor++;
          continue;
        }

      if ((segment_length == 1 && cursor[0] == '.') ||
          (segment_length == 2 && cursor[0] == '.' && cursor[1] == '.') ||
          segment_length > CONFIG_NAME_MAX || memchr(cursor, '\\',
                                                      segment_length) != NULL)
        {
          return -EPERM;
        }

      if (path_used + 1 + segment_length >= path_size ||
          display_used + (display_used > 1 ? 1 : 0) + segment_length >=
          display_size)
        {
          return -ENAMETOOLONG;
        }

      path[path_used++] = '/';
      memcpy(path + path_used, cursor, segment_length);
      path_used += segment_length;
      path[path_used] = '\0';
      if (display_used > 1)
        {
          display[display_used++] = '/';
        }

      memcpy(display + display_used, cursor, segment_length);
      display_used += segment_length;
      display[display_used] = '\0';
      if (slash == NULL)
        {
          break;
        }

      cursor = slash + 1;
    }

  return 0;
}

static bool fm_authorized(const char *target)
{
  char token[FM_TOKEN_SIZE];
  bool authorized;

  if (fm_query_value(target, "key", token, sizeof(token)) < 0)
    {
      return false;
    }

  pthread_mutex_lock(&g_fm.lock);
  authorized = strcmp(token, g_fm.token) == 0;
  pthread_mutex_unlock(&g_fm.lock);
  return authorized;
}

static void fm_list(int fd, const char *target)
{
  char display[FM_PATH_SIZE];
  char path[FM_PATH_SIZE];
  struct fm_stream_s stream =
  {
    .fd = fd,
  };
  DIR *directory;
  struct dirent *entry;
  uint64_t total;
  uint64_t available;
  bool have_space;
  bool first = true;

  if (fm_data_path(target, path, sizeof(path), display,
                   sizeof(display)) < 0)
    {
      fm_send_error(fd, 400, "Bad Request", "无效路径");
      return;
    }

  directory = opendir(path);
  if (directory == NULL)
    {
      fm_send_error(fd, 404, "Not Found", "目录不存在");
      return;
    }

  have_space = fm_storage_space(&total, &available) == 0;

  if (fm_send_header(fd, 200, "OK", "application/json; charset=utf-8", -1,
                     "Cache-Control: no-store\r\n") < 0 ||
      fm_stream_text(&stream, "{\"path\":") < 0 ||
      fm_stream_json_string(&stream, display) < 0 ||
      fm_stream_text(&stream, ",\"items\":[") < 0)
    {
      closedir(directory);
      return;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      char item_path[FM_PATH_SIZE];
      char item[96];
      struct stat info;
      int length;

      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      length = snprintf(item_path, sizeof(item_path), "%s/%s",
                        path, entry->d_name);
      if (length < 0 || (size_t)length >= sizeof(item_path) ||
          stat(item_path, &info) < 0)
        {
          continue;
        }

      if ((!first && fm_stream_text(&stream, ",") < 0) ||
          fm_stream_text(&stream, "{\"name\":") < 0 ||
          fm_stream_json_string(&stream, entry->d_name) < 0)
        {
          break;
        }

      length = snprintf(item, sizeof(item),
                        ",\"dir\":%s,\"size\":%lu}",
                        S_ISDIR(info.st_mode) ? "true" : "false",
                        S_ISREG(info.st_mode) ?
                        (unsigned long)info.st_size : 0ul);
      if (length < 0 || (size_t)length >= sizeof(item) ||
          fm_stream_write(&stream, item, length) < 0)
        {
          break;
        }

      first = false;
    }

  closedir(directory);
  if (have_space)
    {
      char tail[96];
      int length;

      length = snprintf(tail, sizeof(tail),
                        "],\"total\":%llu,\"free\":%llu}",
                        (unsigned long long)total,
                        (unsigned long long)available);
      if (length > 0 && (size_t)length < sizeof(tail))
        {
          fm_stream_write(&stream, tail, length);
        }
      else
        {
          fm_stream_text(&stream, "]}");
        }
    }
  else
    {
      fm_stream_text(&stream, "]}");
    }

  fm_stream_flush(&stream);
}

static void fm_download(int fd, const char *target)
{
  char display[FM_PATH_SIZE];
  char path[FM_PATH_SIZE];
  char buffer[FM_IO_SIZE];
  struct stat info;
  int file;

  if (fm_data_path(target, path, sizeof(path), display,
                   sizeof(display)) < 0 || display[1] == '\0' ||
      stat(path, &info) < 0 || !S_ISREG(info.st_mode))
    {
      fm_send_error(fd, 404, "Not Found", "文件不存在");
      return;
    }

  file = open(path, O_RDONLY);
  if (file < 0)
    {
      fm_send_error(fd, 403, "Forbidden", "无法打开文件");
      return;
    }

  if (fm_send_header(fd, 200, "OK", "application/octet-stream",
                     info.st_size,
                     "Content-Disposition: attachment\r\n"
                     "Cache-Control: no-store\r\n") == 0)
    {
      for (;;)
        {
          ssize_t got = read(file, buffer, sizeof(buffer));

          if (got <= 0 || fm_send_all(fd, buffer, got) < 0)
            {
              break;
            }
        }
    }

  close(file);
}

static void fm_upload(int fd, const char *target,
                      const struct fm_request_s *request,
                      char *request_buffer, size_t received)
{
  char display[FM_PATH_SIZE];
  char path[FM_PATH_SIZE];
  struct timeval timeout;
  uint64_t available;
  uint64_t reclaim = 0;
  size_t remaining;
  size_t initial;
  int file;
  bool failed = false;

  if (!request->has_content_length)
    {
      fm_send_error(fd, 411, "Length Required", "缺少文件大小");
      return;
    }

  if (fm_data_path(target, path, sizeof(path), display,
                   sizeof(display)) < 0 || display[1] == '\0')
    {
      fm_send_error(fd, 400, "Bad Request", "无效文件名");
      return;
    }

  {
    struct stat existing;

    if (lstat(path, &existing) == 0)
      {
        if (!S_ISREG(existing.st_mode))
          {
            fm_send_error(fd, 409, "Conflict", "目标不是普通文件");
            return;
          }

        reclaim = existing.st_size;
      }
  }

  if (fm_storage_space(NULL, &available) < 0)
    {
      fm_send_error(fd, 507, "Insufficient Storage", "无法读取剩余空间");
      return;
    }

  if ((uint64_t)request->content_length > available &&
      (uint64_t)request->content_length - available > reclaim)
    {
      fm_send_error(fd, 507, "Insufficient Storage", "存储空间不足");
      return;
    }

  file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (file < 0)
    {
      fm_send_error(fd, 403, "Forbidden", "无法创建文件");
      return;
    }

  if (request->expect_continue)
    {
      fm_send_text(fd, "HTTP/1.1 100 Continue\r\n\r\n");
    }

  timeout.tv_sec = FM_UPLOAD_TIMEOUT;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  remaining = request->content_length;
  initial = received > request->body_offset ?
            received - request->body_offset : 0;
  if (initial > remaining)
    {
      initial = remaining;
    }

  if (initial > 0 && fm_write_all(file,
                                  request_buffer + request->body_offset,
                                  initial) < 0)
    {
      failed = true;
    }

  remaining -= initial;
  while (!failed && remaining > 0)
    {
      size_t wanted = remaining < FM_HEADER_SIZE ? remaining : FM_HEADER_SIZE;
      size_t filled = 0;

      while (filled < wanted)
        {
          ssize_t got = recv(fd, request_buffer + filled,
                             wanted - filled, 0);

          if (got < 0 && errno == EINTR)
            {
              continue;
            }

          if (got <= 0)
            {
              failed = true;
              break;
            }

          filled += got;
        }

      if (!failed && fm_write_all(file, request_buffer, filled) < 0)
        {
          failed = true;
        }

      remaining -= filled;
    }

  if (close(file) < 0)
    {
      failed = true;
    }

  if (failed)
    {
      unlink(path);
      fm_send_error(fd, 507, "Insufficient Storage", "上传失败或空间不足");
      return;
    }

  fm_send_header(fd, 204, "No Content", "text/plain", 0,
                 "Cache-Control: no-store\r\n");
}

static void fm_mkdir(int fd, const char *target)
{
  char display[FM_PATH_SIZE];
  char path[FM_PATH_SIZE];

  if (fm_data_path(target, path, sizeof(path), display,
                   sizeof(display)) < 0 || display[1] == '\0')
    {
      fm_send_error(fd, 400, "Bad Request", "无效目录名");
      return;
    }

  if (mkdir(path, 0777) < 0)
    {
      fm_send_error(fd, errno == EEXIST ? 409 : 403,
                    errno == EEXIST ? "Conflict" : "Forbidden",
                    errno == EEXIST ? "目录已存在" : "无法创建目录");
      return;
    }

  fm_send_header(fd, 204, "No Content", "text/plain", 0,
                 "Cache-Control: no-store\r\n");
}

static void fm_delete(int fd, const char *target)
{
  char display[FM_PATH_SIZE];
  char path[FM_PATH_SIZE];
  struct stat info;
  int ret;

  if (fm_data_path(target, path, sizeof(path), display,
                   sizeof(display)) < 0 || display[1] == '\0' ||
      lstat(path, &info) < 0)
    {
      fm_send_error(fd, 400, "Bad Request", "无效路径");
      return;
    }

  ret = S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path);
  if (ret < 0)
    {
      fm_send_error(fd, errno == ENOTEMPTY ? 409 : 403,
                    errno == ENOTEMPTY ? "Conflict" : "Forbidden",
                    errno == ENOTEMPTY ? "目录不是空的" : "无法删除");
      return;
    }

  fm_send_header(fd, 204, "No Content", "text/plain", 0,
                 "Cache-Control: no-store\r\n");
}

static void fm_handle(int fd)
{
  char buffer[FM_HEADER_SIZE];
  struct fm_request_s request;
  struct timeval timeout;
  size_t received;
  char *query;
  int ret;

  timeout.tv_sec = 15;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  ret = fm_read_request(fd, buffer, sizeof(buffer), &received, &request);
  if (ret < 0)
    {
      fm_send_error(fd, ret == -E2BIG ? 431 :
                        ret == -EFBIG ? 413 : 400,
                    ret == -E2BIG ? "Request Header Fields Too Large" :
                    ret == -EFBIG ? "Content Too Large" : "Bad Request",
                    ret == -EFBIG ? "文件太大" : "请求格式错误");
      return;
    }

  query = strchr(request.target, '?');
  if (strcmp(request.method, "GET") == 0 &&
      ((query != NULL && query == request.target + 1) ||
       strcmp(request.target, "/") == 0))
    {
      fm_send_header(fd, 200, "OK", "text/html; charset=utf-8",
                     sizeof(g_fm_html) - 1,
                     "Cache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Content-Security-Policy: default-src 'self'; "
                     "script-src 'unsafe-inline'; style-src 'unsafe-inline'\r\n");
      fm_send_all(fd, g_fm_html, sizeof(g_fm_html) - 1);
      return;
    }

  if (!fm_authorized(request.target))
    {
      fm_send_error(fd, 401, "Unauthorized", "访问码错误");
      return;
    }

  if (strcmp(request.method, "GET") == 0 &&
      strncmp(request.target, "/api/list?", 10) == 0)
    {
      fm_list(fd, request.target);
    }
  else if (strcmp(request.method, "GET") == 0 &&
           strncmp(request.target, "/api/download?", 14) == 0)
    {
      fm_download(fd, request.target);
    }
  else if (strcmp(request.method, "PUT") == 0 &&
           strncmp(request.target, "/api/upload?", 12) == 0)
    {
      fm_upload(fd, request.target, &request, buffer, received);
    }
  else if (strcmp(request.method, "POST") == 0 &&
           strncmp(request.target, "/api/mkdir?", 11) == 0)
    {
      fm_mkdir(fd, request.target);
    }
  else if (strcmp(request.method, "DELETE") == 0 &&
           strncmp(request.target, "/api/delete?", 12) == 0)
    {
      fm_delete(fd, request.target);
    }
  else
    {
      fm_send_error(fd, 404, "Not Found", "接口不存在");
    }
}

static void *fm_worker(void *arg)
{
  int listen_fd = (int)(intptr_t)arg;

  for (;;)
    {
      int client = accept(listen_fd, NULL, NULL);

      if (client < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      fm_handle(client);
      close(client);
    }

  pthread_mutex_lock(&g_fm.lock);
  if (g_fm.listen_fd == listen_fd)
    {
      close(g_fm.listen_fd);
      g_fm.listen_fd = -1;
      g_fm.running = false;
    }

  pthread_mutex_unlock(&g_fm.lock);
  return NULL;
}

int desktop_filemgr_start(void)
{
  struct sockaddr_in address;
  struct timespec now;
  pthread_attr_t attr;
  pthread_t thread;
  bool attr_initialized = false;
  int option = 1;
  int listen_fd;
  int ret;

  pthread_mutex_lock(&g_fm.lock);
  if (g_fm.running)
    {
      pthread_mutex_unlock(&g_fm.lock);
      return 0;
    }

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    {
      ret = -errno;
      pthread_mutex_unlock(&g_fm.lock);
      return ret;
    }

  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(CONFIG_SYSTEM_DESKTOP_FILEMGR_PORT);
  if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      listen(listen_fd, 2) < 0)
    {
      ret = -errno;
      close(listen_fd);
      pthread_mutex_unlock(&g_fm.lock);
      return ret;
    }

  clock_gettime(CLOCK_MONOTONIC, &now);
  snprintf(g_fm.token, sizeof(g_fm.token), "%06lu",
           (unsigned long)((now.tv_nsec ^ now.tv_sec ^ (uintptr_t)&g_fm) %
                           1000000));
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_initialized = true;
      ret = pthread_attr_setstacksize(&attr, FM_WORKER_STACK);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }

  if (ret == 0)
    {
      g_fm.listen_fd = listen_fd;
      g_fm.running = true;
      ret = pthread_create(&thread, &attr, fm_worker,
                           (void *)(intptr_t)listen_fd);
      if (ret != 0)
        {
          g_fm.listen_fd = -1;
          g_fm.running = false;
        }
    }

  if (attr_initialized)
    {
      pthread_attr_destroy(&attr);
    }
  if (ret != 0)
    {
      close(listen_fd);
      ret = -ret;
    }

  pthread_mutex_unlock(&g_fm.lock);
  return ret;
}

bool desktop_filemgr_running(void)
{
  bool running;

  pthread_mutex_lock(&g_fm.lock);
  running = g_fm.running;
  pthread_mutex_unlock(&g_fm.lock);
  return running;
}

int desktop_filemgr_get_token(char *buffer, size_t buffer_size)
{
  int ret = 0;

  if (buffer == NULL || buffer_size < sizeof(g_fm.token))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_fm.lock);
  if (!g_fm.running)
    {
      ret = -ENODEV;
    }
  else
    {
      strlcpy(buffer, g_fm.token, buffer_size);
    }

  pthread_mutex_unlock(&g_fm.lock);
  return ret;
}
