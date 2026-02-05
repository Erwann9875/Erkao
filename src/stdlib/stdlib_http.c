#include "stdlib_internal.h"
#include "gc.h"

#include <errno.h>
#include <math.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wchar.h>
#else
#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t* utf8ToWide(const char* chars, int length) {
  int needed = MultiByteToWideChar(CP_UTF8, 0, chars, length, NULL, 0);
  if (needed <= 0) return NULL;
  int alloc = length == -1 ? needed : needed + 1;
  wchar_t* buffer = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)alloc);
  if (!buffer) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, chars, length, buffer, needed);
  if (length != -1) {
    buffer[needed] = L'\0';
  }
  return buffer;
}

static wchar_t* wideSubstring(const wchar_t* start, DWORD length) {
  wchar_t* buffer = (wchar_t*)malloc(sizeof(wchar_t) * ((size_t)length + 1));
  if (!buffer) return NULL;
  if (length > 0) {
    wmemcpy(buffer, start, length);
  }
  buffer[length] = L'\0';
  return buffer;
}

static char* wideToUtf8(const wchar_t* chars, int length, int* outLength) {
  int needed = WideCharToMultiByte(CP_UTF8, 0, chars, length, NULL, 0, NULL, NULL);
  if (needed <= 0) return NULL;
  int alloc = length == -1 ? needed : needed + 1;
  char* buffer = (char*)malloc((size_t)alloc);
  if (!buffer) return NULL;
  WideCharToMultiByte(CP_UTF8, 0, chars, length, buffer, needed, NULL, NULL);
  if (length == -1) {
    buffer[needed - 1] = '\0';
    if (outLength) *outLength = needed - 1;
  } else {
    buffer[needed] = '\0';
    if (outLength) *outLength = needed;
  }
  return buffer;
}

static Value httpRequest(VM* vm, const char* method, ObjString* url,
                         const char* body, size_t bodyLength, const char* message) {
  Value result = NULL_VAL;
  bool ok = false;
  ByteBuffer bodyBuffer;
  bufferInit(&bodyBuffer);
  wchar_t* headerWide = NULL;

  wchar_t* wideUrl = utf8ToWide(url->chars, -1);
  if (!wideUrl) return runtimeErrorValue(vm, message);

  URL_COMPONENTS parts;
  memset(&parts, 0, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = (DWORD)-1;
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;

  if (!WinHttpCrackUrl(wideUrl, 0, 0, &parts)) {
    free(wideUrl);
    return runtimeErrorValue(vm, message);
  }

  wchar_t* host = wideSubstring(parts.lpszHostName, parts.dwHostNameLength);
  if (!host) {
    free(wideUrl);
    return runtimeErrorValue(vm, message);
  }
  DWORD pathLength = parts.dwUrlPathLength + parts.dwExtraInfoLength;
  wchar_t* path = NULL;
  if (pathLength == 0) {
    path = wideSubstring(L"/", 1);
  } else {
    path = (wchar_t*)malloc(sizeof(wchar_t) * ((size_t)pathLength + 1));
    if (!path) {
      free(wideUrl);
      free(host);
      return runtimeErrorValue(vm, message);
    }
    if (parts.dwUrlPathLength > 0) {
      wmemcpy(path, parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.dwExtraInfoLength > 0) {
      wmemcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    path[pathLength] = L'\0';
  }
  if (!path) {
    free(wideUrl);
    free(host);
    return runtimeErrorValue(vm, message);
  }

  wchar_t* wideMethod = utf8ToWide(method, -1);
  if (!wideMethod) {
    free(wideUrl);
    free(host);
    free(path);
    return runtimeErrorValue(vm, message);
  }

  HINTERNET session = WinHttpOpen(L"Erkao/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) goto cleanup;

  HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    goto cleanup;
  }

  DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(connect, wideMethod, path, NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    goto cleanup;
  }

  BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 bodyLength > 0 ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                                 (DWORD)bodyLength, (DWORD)bodyLength, 0);
  if (!sent) goto request_cleanup;

  if (!WinHttpReceiveResponse(request, NULL)) goto request_cleanup;

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                      WINHTTP_NO_HEADER_INDEX);

  DWORD headerSize = 0;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                           WINHTTP_HEADER_NAME_BY_INDEX, NULL, &headerSize,
                           WINHTTP_NO_HEADER_INDEX)) {
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headerSize > 0) {
      headerWide = (wchar_t*)malloc(headerSize);
      if (!headerWide) {
        goto request_cleanup;
      }
      WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                          WINHTTP_HEADER_NAME_BY_INDEX, headerWide, &headerSize,
                          WINHTTP_NO_HEADER_INDEX);
    }
  }

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) break;
    if (available == 0) break;

    char* chunk = (char*)malloc(available);
    if (!chunk) {
      goto request_cleanup;
    }
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk, available, &read)) {
      free(chunk);
      break;
    }
    if (read > 0) {
      bufferAppendN(&bodyBuffer, chunk, read);
      if (bodyBuffer.failed) {
        free(chunk);
        goto request_cleanup;
      }
    }
    free(chunk);
  }

  ObjMap* response = newMap(vm);
  if (!response) {
    goto request_cleanup;
  }
  mapSet(response, copyString(vm, "status"), NUMBER_VAL((double)status));
  mapSet(response, copyString(vm, "body"),
         OBJ_VAL(copyStringWithLength(vm,
                                      bodyBuffer.data ? bodyBuffer.data : "",
                                      (int)bodyBuffer.length)));

  if (headerWide) {
    int headerLength = 0;
    char* headerUtf8 = wideToUtf8(headerWide, -1, &headerLength);
    if (headerUtf8) {
      mapSet(response, copyString(vm, "headers"),
             OBJ_VAL(copyStringWithLength(vm, headerUtf8, headerLength)));
      free(headerUtf8);
    } else {
      mapSet(response, copyString(vm, "headers"), OBJ_VAL(copyString(vm, "")));
    }
    free(headerWide);
    headerWide = NULL;
  } else {
    mapSet(response, copyString(vm, "headers"), OBJ_VAL(copyString(vm, "")));
  }

  result = OBJ_VAL(response);
  ok = true;

request_cleanup:
  if (headerWide) {
    free(headerWide);
    headerWide = NULL;
  }
  bufferFree(&bodyBuffer);
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

cleanup:
  free(wideUrl);
  free(host);
  free(path);
  free(wideMethod);
  if (!ok) {
    return runtimeErrorValue(vm, message);
  }
  return result;
}

static Value nativeHttpGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.get expects a url string.");
  }
  return httpRequest(vm, "GET", (ObjString*)AS_OBJ(args[0]), NULL, 0, "http.get failed.");
}

static Value nativeHttpPost(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.post expects (url, body) strings.");
  }
  ObjString* body = (ObjString*)AS_OBJ(args[1]);
  return httpRequest(vm, "POST", (ObjString*)AS_OBJ(args[0]),
                     body->chars, (size_t)body->length, "http.post failed.");
}

static Value nativeHttpRequest(VM* vm, int argc, Value* args) {
  if (argc < 2 || argc > 3) {
    return runtimeErrorValue(vm, "http.request expects (method, url[, body]).");
  }
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.request expects (method, url[, body]).");
  }
  const char* body = NULL;
  size_t bodyLength = 0;
  if (argc >= 3 && !IS_NULL(args[2])) {
    if (!isObjType(args[2], OBJ_STRING)) {
      return runtimeErrorValue(vm, "http.request expects body to be a string or null.");
    }
    ObjString* bodyString = (ObjString*)AS_OBJ(args[2]);
    body = bodyString->chars;
    bodyLength = (size_t)bodyString->length;
  }
  ObjString* method = (ObjString*)AS_OBJ(args[0]);
  return httpRequest(vm, method->chars, (ObjString*)AS_OBJ(args[1]),
                     body, bodyLength, "http.request failed.");
}
#else
static bool httpEnsureCurl(void) {
  static bool initialized = false;
  static bool ok = false;
  if (!initialized) {
    ok = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    initialized = true;
  }
  return ok;
}

static size_t httpWriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
  size_t total = size * nmemb;
  ByteBuffer* buffer = (ByteBuffer*)userp;
  bufferAppendN(buffer, contents, total);
  if (buffer->failed) return 0;
  return total;
}

static Value httpRequest(VM* vm, const char* method, ObjString* url,
                         const char* body, size_t bodyLength, const char* message) {
  if (!httpEnsureCurl()) {
    return runtimeErrorValue(vm, message);
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    return runtimeErrorValue(vm, message);
  }

  ByteBuffer bodyBuffer;
  ByteBuffer headerBuffer;
  bufferInit(&bodyBuffer);
  bufferInit(&headerBuffer);

  curl_easy_setopt(curl, CURLOPT_URL, url->chars);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Erkao/1.0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, httpWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyBuffer);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, httpWriteCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerBuffer);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  bool isGet = strcmp(method, "GET") == 0;
  bool isPost = strcmp(method, "POST") == 0;

  if (isPost) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else if (!isGet || bodyLength > 0) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }

  if (bodyLength > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodyLength);
  } else if (isPost) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
  }

  CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK || bodyBuffer.failed || headerBuffer.failed) {
    curl_easy_cleanup(curl);
    bufferFree(&bodyBuffer);
    bufferFree(&headerBuffer);
    return runtimeErrorValue(vm, message);
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  ObjMap* response = newMap(vm);
  if (!response) {
    curl_easy_cleanup(curl);
    bufferFree(&bodyBuffer);
    bufferFree(&headerBuffer);
    return runtimeErrorValue(vm, message);
  }
  mapSet(response, copyString(vm, "status"), NUMBER_VAL((double)status));
  mapSet(response, copyString(vm, "body"),
         OBJ_VAL(copyStringWithLength(vm,
                                      bodyBuffer.data ? bodyBuffer.data : "",
                                      (int)bodyBuffer.length)));
  mapSet(response, copyString(vm, "headers"),
         OBJ_VAL(copyStringWithLength(vm,
                                      headerBuffer.data ? headerBuffer.data : "",
                                      (int)headerBuffer.length)));

  curl_easy_cleanup(curl);
  bufferFree(&bodyBuffer);
  bufferFree(&headerBuffer);
  return OBJ_VAL(response);
}

static Value nativeHttpGet(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.get expects a url string.");
  }
  return httpRequest(vm, "GET", (ObjString*)AS_OBJ(args[0]), NULL, 0, "http.get failed.");
}

static Value nativeHttpPost(VM* vm, int argc, Value* args) {
  (void)argc;
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.post expects (url, body) strings.");
  }
  ObjString* body = (ObjString*)AS_OBJ(args[1]);
  return httpRequest(vm, "POST", (ObjString*)AS_OBJ(args[0]),
                     body->chars, (size_t)body->length, "http.post failed.");
}

static Value nativeHttpRequest(VM* vm, int argc, Value* args) {
  if (argc < 2 || argc > 3) {
    return runtimeErrorValue(vm, "http.request expects (method, url[, body]).");
  }
  if (!isObjType(args[0], OBJ_STRING) || !isObjType(args[1], OBJ_STRING)) {
    return runtimeErrorValue(vm, "http.request expects (method, url[, body]).");
  }
  const char* body = NULL;
  size_t bodyLength = 0;
  if (argc >= 3 && !IS_NULL(args[2])) {
    if (!isObjType(args[2], OBJ_STRING)) {
      return runtimeErrorValue(vm, "http.request expects body to be a string or null.");
    }
    ObjString* bodyString = (ObjString*)AS_OBJ(args[2]);
    body = bodyString->chars;
    bodyLength = (size_t)bodyString->length;
  }
  ObjString* method = (ObjString*)AS_OBJ(args[0]);
  return httpRequest(vm, method->chars, (ObjString*)AS_OBJ(args[1]),
                     body, bodyLength, "http.request failed.");
}
#endif

#define HTTP_MAX_REQUEST_BYTES 65536
#define HTTP_CLIENT_TIMEOUT_MS 5000
#define HTTP_ACCEPT_TIMEOUT_MS 250

#ifdef _WIN32
typedef SOCKET ErkaoSocket;
#define ERKAO_INVALID_SOCKET INVALID_SOCKET
#define ERKAO_SOCKET_ERROR SOCKET_ERROR
#define erkaoCloseSocket closesocket
#else
typedef int ErkaoSocket;
#define ERKAO_INVALID_SOCKET (-1)
#define ERKAO_SOCKET_ERROR (-1)
#define erkaoCloseSocket close
#endif

static bool httpSetSocketTimeouts(ErkaoSocket socket, int timeoutMs) {
#ifdef _WIN32
  DWORD timeout = (DWORD)timeoutMs;
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                 (const char*)&timeout, (int)sizeof(timeout)) == ERKAO_SOCKET_ERROR) {
    return false;
  }
  if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                 (const char*)&timeout, (int)sizeof(timeout)) == ERKAO_SOCKET_ERROR) {
    return false;
  }
#else
  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                 &timeout, (socklen_t)sizeof(timeout)) == ERKAO_SOCKET_ERROR) {
    return false;
  }
  if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                 &timeout, (socklen_t)sizeof(timeout)) == ERKAO_SOCKET_ERROR) {
    return false;
  }
#endif
  return true;
}

static bool httpWaitForClient(ErkaoSocket server, int timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(server, &readSet);

  struct timeval timeout;
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
  int ready = select(0, &readSet, NULL, NULL, &timeout);
#else
  int ready = select(server + 1, &readSet, NULL, NULL, &timeout);
#endif
  if (ready > 0) return true;
  if (ready == 0) return false;

#ifndef _WIN32
  if (errno == EINTR) return false;
#endif
  return false;
}

static bool httpSocketStartup(void) {
#ifdef _WIN32
  static bool started = false;
  if (!started) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
    started = true;
  }
#endif
  return true;
}

static bool httpSocketAddrInUse(void) {
#ifdef _WIN32
  return WSAGetLastError() == WSAEADDRINUSE;
#else
  return errno == EADDRINUSE;
#endif
}

static int httpSocketGetPort(ErkaoSocket server) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
#ifdef _WIN32
  int len = (int)sizeof(addr);
  if (getsockname(server, (struct sockaddr*)&addr, &len) == SOCKET_ERROR) {
    return -1;
  }
#else
  socklen_t len = sizeof(addr);
  if (getsockname(server, (struct sockaddr*)&addr, &len) < 0) {
    return -1;
  }
#endif
  return (int)ntohs(addr.sin_port);
}

static bool httpBindServerSocket(ErkaoSocket* out, int port, int* outPort, bool* outInUse) {
  if (outInUse) *outInUse = false;

  ErkaoSocket server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == ERKAO_INVALID_SOCKET) {
    return false;
  }

  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
  setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&opt, sizeof(opt));
#endif

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);

  if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) == ERKAO_SOCKET_ERROR) {
    if (outInUse) *outInUse = httpSocketAddrInUse();
    erkaoCloseSocket(server);
    return false;
  }

  if (listen(server, SOMAXCONN) == ERKAO_SOCKET_ERROR) {
    erkaoCloseSocket(server);
    return false;
  }

  int boundPort = httpSocketGetPort(server);
  if (boundPort <= 0) {
    erkaoCloseSocket(server);
    return false;
  }

  *out = server;
  *outPort = boundPort;
  return true;
}

static bool httpPortFromValue(VM* vm, Value value, int* outPort) {
  if (IS_NULL(value)) {
    *outPort = 0;
    return true;
  }
  if (!IS_NUMBER(value)) {
    runtimeErrorValue(vm, "http.serve expects port to be a number or null.");
    return false;
  }
  double number = AS_NUMBER(value);
  double truncated = floor(number);
  if (number != truncated) {
    runtimeErrorValue(vm, "http.serve expects port to be an integer.");
    return false;
  }
  if (number < 0.0 || number > 65535.0) {
    runtimeErrorValue(vm, "http.serve expects port in range 0-65535.");
    return false;
  }
  *outPort = (int)number;
  return true;
}

static bool httpFindHeaderEnd(const char* data, size_t length, size_t* outIndex) {
  if (length < 2) return false;
  for (size_t i = 3; i < length; i++) {
    if (data[i - 3] == '\r' && data[i - 2] == '\n' &&
        data[i - 1] == '\r' && data[i] == '\n') {
      *outIndex = i + 1;
      return true;
    }
  }
  for (size_t i = 1; i < length; i++) {
    if (data[i - 1] == '\n' && data[i] == '\n') {
      *outIndex = i + 1;
      return true;
    }
  }
  return false;
}

static bool httpReadHeaders(ErkaoSocket client, ByteBuffer* buffer, size_t* headerEnd) {
  char chunk[1024];
  while (buffer->length < HTTP_MAX_REQUEST_BYTES) {
    int received = recv(client, chunk, (int)sizeof(chunk), 0);
    if (received <= 0) {
      return false;
    }
    bufferAppendN(buffer, chunk, (size_t)received);
    if (buffer->failed) {
      return false;
    }
    if (httpFindHeaderEnd(buffer->data, buffer->length, headerEnd)) {
      return true;
    }
  }
  return false;
}

static bool httpParseRequestLine(const char* data, size_t headerEnd,
                                 const char** method, size_t* methodLen,
                                 const char** path, size_t* pathLen) {
  if (!data || headerEnd == 0) return false;
  const char* lineEnd = memchr(data, '\n', headerEnd);
  if (!lineEnd) return false;
  const char* lineEndClean = lineEnd;
  if (lineEndClean > data && lineEndClean[-1] == '\r') {
    lineEndClean--;
  }
  const char* space1 = memchr(data, ' ', (size_t)(lineEndClean - data));
  if (!space1) return false;
  const char* space2 = memchr(space1 + 1, ' ', (size_t)(lineEndClean - (space1 + 1)));
  if (!space2) return false;
  if (space1 == data || space2 == space1 + 1) return false;
  *method = data;
  *methodLen = (size_t)(space1 - data);
  *path = space1 + 1;
  *pathLen = (size_t)(space2 - (space1 + 1));
  return true;
}

static bool httpStringEqualsIgnoreCaseN(const char* left, int leftLen, const char* right) {
  int rightLen = (int)strlen(right);
  if (leftLen != rightLen) return false;
  for (int i = 0; i < leftLen; i++) {
    unsigned char a = (unsigned char)left[i];
    unsigned char b = (unsigned char)right[i];
    if (tolower(a) != tolower(b)) return false;
  }
  return true;
}

static bool httpHeaderTokenChar(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

static bool httpHeaderNameSafe(const char* name) {
  if (!name || name[0] == '\0') return false;
  for (const char* c = name; *c; c++) {
    if (!httpHeaderTokenChar(*c)) return false;
  }
  return true;
}

static bool httpHeaderValueSafe(const char* value) {
  if (!value) return false;
  for (const char* c = value; *c; c++) {
    if (*c == '\r' || *c == '\n') return false;
  }
  return true;
}

static bool httpAppendHeader(ByteBuffer* buffer, const char* name, const char* value) {
  if (!httpHeaderNameSafe(name) || !httpHeaderValueSafe(value)) return false;
  bufferAppendN(buffer, name, strlen(name));
  bufferAppendN(buffer, ": ", 2);
  bufferAppendN(buffer, value, strlen(value));
  bufferAppendN(buffer, "\r\n", 2);
  if (buffer->failed) return false;
  return true;
}

static const char* httpStatusText(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 408:
      return "Request Timeout";
    case 413:
      return "Payload Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "OK";
  }
}

static bool httpAppendHeadersFromMap(ByteBuffer* buffer, ObjMap* headers, bool* hasContentType) {
  if (!headers) return true;
  for (int i = 0; i < headers->capacity; i++) {
    MapEntryValue* entry = &headers->entries[i];
    if (!entry->key) continue;
    if (!isObjType(entry->value, OBJ_STRING)) continue;
    ObjString* key = entry->key;
    ObjString* value = (ObjString*)AS_OBJ(entry->value);
    if (!httpHeaderNameSafe(key->chars) || !httpHeaderValueSafe(value->chars)) {
      continue;
    }
    if (httpStringEqualsIgnoreCaseN(key->chars, key->length, "Content-Type")) {
      if (hasContentType) *hasContentType = true;
    }
    if (!httpAppendHeader(buffer, key->chars, value->chars)) return false;
  }
  return true;
}

static bool httpSendAll(ErkaoSocket client, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    int chunk = (int)(length - sent);
    int wrote = send(client, data + sent, chunk, 0);
    if (wrote <= 0) return false;
    sent += (size_t)wrote;
  }
  return true;
}

static bool httpSendResponse(ErkaoSocket client, int status, const char* body,
                             size_t bodyLength, ObjMap* headers, ObjMap* corsConfig) {
  ByteBuffer response;
  bufferInit(&response);

  char statusLine[64];
  const char* statusText = httpStatusText(status);
  int statusLen = snprintf(statusLine, sizeof(statusLine),
                           "HTTP/1.1 %d %s\r\n", status, statusText);
  if (statusLen < 0) statusLen = 0;
  bufferAppendN(&response, statusLine, (size_t)statusLen);
  if (response.failed) {
    bufferFree(&response);
    return false;
  }

  bool hasContentType = false;
  if (!httpAppendHeadersFromMap(&response, headers, &hasContentType)) {
    bufferFree(&response);
    return false;
  }
  if (!hasContentType) {
    if (!httpAppendHeader(&response, "Content-Type", "text/plain; charset=utf-8")) {
      bufferFree(&response);
      return false;
    }
  }

  if (corsConfig) {
    Value originVal;
    ObjString* originKey = copyString(corsConfig->vm, "origin");
    if (mapGet(corsConfig, originKey, &originVal) && isObjType(originVal, OBJ_STRING)) {
      ObjString* origin = (ObjString*)AS_OBJ(originVal);
      (void)httpAppendHeader(&response, "Access-Control-Allow-Origin", origin->chars);
    }
    
    Value methodsVal;
    ObjString* methodsKey = copyString(corsConfig->vm, "methods");
    if (mapGet(corsConfig, methodsKey, &methodsVal) && isObjType(methodsVal, OBJ_STRING)) {
      ObjString* methods = (ObjString*)AS_OBJ(methodsVal);
      (void)httpAppendHeader(&response, "Access-Control-Allow-Methods", methods->chars);
    }
    
    Value headersVal;
    ObjString* headersKey = copyString(corsConfig->vm, "headers");
    if (mapGet(corsConfig, headersKey, &headersVal) && isObjType(headersVal, OBJ_STRING)) {
      ObjString* hdrs = (ObjString*)AS_OBJ(headersVal);
      (void)httpAppendHeader(&response, "Access-Control-Allow-Headers", hdrs->chars);
    }
  }

  char lengthValue[64];
  int lengthLen = snprintf(lengthValue, sizeof(lengthValue), "%zu", bodyLength);
  if (lengthLen < 0) lengthLen = 0;
  if (!httpAppendHeader(&response, "Content-Length", lengthValue)) {
    bufferFree(&response);
    return false;
  }
  if (!httpAppendHeader(&response, "Connection", "close")) {
    bufferFree(&response);
    return false;
  }
  bufferAppendN(&response, "\r\n", 2);
  if (response.failed) {
    bufferFree(&response);
    return false;
  }

  if (bodyLength > 0 && body) {
    bufferAppendN(&response, body, bodyLength);
    if (response.failed) {
      bufferFree(&response);
      return false;
    }
  }

  bool ok = httpSendAll(client,
                        response.data ? response.data : "",
                        response.length);
  bufferFree(&response);
  return ok;
}

static void httpLogRequest(const struct sockaddr_in* addr,
                           const char* path, size_t pathLen) {
  char ip[INET_ADDRSTRLEN] = "unknown";
#ifdef _WIN32
  if (addr) {
    InetNtopA(AF_INET, (void*)&addr->sin_addr, ip, (DWORD)sizeof(ip));
  }
#else
  if (addr) {
    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
      snprintf(ip, sizeof(ip), "unknown");
    }
  }
#endif

  char timeBuf[32] = {0};
  time_t now = time(NULL);
  struct tm localTime;
#ifdef _WIN32
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif
  strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &localTime);

  if (pathLen == 0 || !path) {
    printf("[%s] [%s] Called /\n", ip, timeBuf);
  } else {
    printf("[%s] [%s] Called %.*s\n", ip, timeBuf, (int)pathLen, path);
  }
  fflush(stdout);
}

static long httpGetContentLength(const char* headers, size_t headerEnd) {
  const char* clHeader = "Content-Length:";
  size_t clLen = strlen(clHeader);
  const char* cursor = headers;
  const char* end = headers + headerEnd;
  while (cursor < end) {
    const char* lineEnd = memchr(cursor, '\n', (size_t)(end - cursor));
    if (!lineEnd) break;
    size_t lineLen = (size_t)(lineEnd - cursor);
    if (lineLen > 0 && cursor[lineLen - 1] == '\r') lineLen--;
    if (lineLen > clLen && httpStringEqualsIgnoreCaseN(cursor, (int)clLen, clHeader)) {
      const char* value = cursor + clLen;
      while (*value == ' ' && value < lineEnd) value++;
      long length = strtol(value, NULL, 10);
      return length > 0 ? length : 0;
    }
    cursor = lineEnd + 1;
  }
  return 0;
}

static ObjMap* httpParseHeaders(VM* vm, const char* data, size_t headerEnd) {
  ObjMap* headers = newMap(vm);
  const char* cursor = data;
  const char* end = data + headerEnd;
  
  const char* firstLine = memchr(cursor, '\n', (size_t)(end - cursor));
  if (firstLine) cursor = firstLine + 1;
  
  while (cursor < end) {
    const char* lineEnd = memchr(cursor, '\n', (size_t)(end - cursor));
    if (!lineEnd) break;
    size_t lineLen = (size_t)(lineEnd - cursor);
    if (lineLen > 0 && cursor[lineLen - 1] == '\r') lineLen--;
    if (lineLen == 0) break;
    
    const char* colon = memchr(cursor, ':', lineLen);
    if (colon && colon > cursor) {
      size_t keyLen = (size_t)(colon - cursor);
      const char* value = colon + 1;
      while (*value == ' ' && value < cursor + lineLen) value++;
      size_t valueLen = lineLen - (size_t)(value - cursor);
      
      ObjString* key = copyStringWithLength(vm, cursor, (int)keyLen);
      ObjString* val = copyStringWithLength(vm, value, (int)valueLen);
      mapSet(headers, key, OBJ_VAL(val));
    }
    cursor = lineEnd + 1;
  }
  return headers;
}

static bool httpReadBody(ErkaoSocket client, ByteBuffer* buffer, size_t headerEnd, long contentLength) {
  if (contentLength <= 0) return true;
  if (headerEnd >= HTTP_MAX_REQUEST_BYTES) return false;
  size_t maxBody = HTTP_MAX_REQUEST_BYTES - headerEnd;
  if ((size_t)contentLength > maxBody) return false;

  size_t alreadyRead = buffer->length > headerEnd ? buffer->length - headerEnd : 0;
  size_t remaining = (size_t)contentLength > alreadyRead ? (size_t)contentLength - alreadyRead : 0;
  
  char chunk[1024];
  while (remaining > 0) {
    if (buffer->length >= HTTP_MAX_REQUEST_BYTES) {
      return false;
    }
    size_t toRead = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    int received = recv(client, chunk, (int)toRead, 0);
    if (received <= 0) return false;
    bufferAppendN(buffer, chunk, (size_t)received);
    if (buffer->failed) return false;
    remaining -= (size_t)received;
  }
  return remaining == 0;
}

static ObjMap* httpCreateRequestObject(VM* vm, const char* method, size_t methodLen,
                                        const char* path, size_t pathLen,
                                        ObjMap* headers, const char* body, size_t bodyLen) {
  ObjMap* request = newMap(vm);
  
  ObjString* methodKey = copyString(vm, "method");
  ObjString* methodVal = copyStringWithLength(vm, method, (int)methodLen);
  mapSet(request, methodKey, OBJ_VAL(methodVal));
  
  ObjString* pathKey = copyString(vm, "path");
  ObjString* pathVal = copyStringWithLength(vm, path, (int)pathLen);
  mapSet(request, pathKey, OBJ_VAL(pathVal));
  
  ObjString* headersKey = copyString(vm, "headers");
  mapSet(request, headersKey, OBJ_VAL(headers));
  
  ObjString* bodyKey = copyString(vm, "body");
  ObjString* bodyVal = copyStringWithLength(vm, body ? body : "", body ? (int)bodyLen : 0);
  mapSet(request, bodyKey, OBJ_VAL(bodyVal));
  
  return request;
}

static bool httpResponseFromValue(VM* vm, Value value, int* statusOut,
                                  const char** bodyOut, size_t* bodyLenOut,
                                  ObjMap** headersOut, ObjMap* requestObj) {
  *statusOut = 200;
  *bodyOut = "";
  *bodyLenOut = 0;
  *headersOut = NULL;

  if (isObjType(value, OBJ_FUNCTION) || isObjType(value, OBJ_BOUND_METHOD)) {
    if (!requestObj) {
      return false;
    }
    Value request = OBJ_VAL(requestObj);
    Value result;
    if (!vmCallValue(vm, value, 1, &request, &result)) {
      return false;
    }
    return httpResponseFromValue(vm, result, statusOut, bodyOut, bodyLenOut, headersOut, NULL);
  }

  if (isObjType(value, OBJ_STRING)) {
    ObjString* body = (ObjString*)AS_OBJ(value);
    *bodyOut = body->chars;
    *bodyLenOut = (size_t)body->length;
    return true;
  }

  if (isObjType(value, OBJ_MAP)) {
    ObjMap* response = (ObjMap*)AS_OBJ(value);
    Value statusValue;
    ObjString* statusKey = copyString(vm, "status");
    if (mapGet(response, statusKey, &statusValue)) {
      if (!IS_NUMBER(statusValue)) return false;
      double statusNumber = AS_NUMBER(statusValue);
      double truncated = floor(statusNumber);
      if (statusNumber != truncated || statusNumber < 100.0 || statusNumber > 599.0) {
        return false;
      }
      *statusOut = (int)statusNumber;
    }

    Value bodyValue;
    ObjString* bodyKey = copyString(vm, "body");
    if (mapGet(response, bodyKey, &bodyValue)) {
      if (!isObjType(bodyValue, OBJ_STRING)) return false;
      ObjString* body = (ObjString*)AS_OBJ(bodyValue);
      *bodyOut = body->chars;
      *bodyLenOut = (size_t)body->length;
    }

    Value headersValue;
    ObjString* headersKey = copyString(vm, "headers");
    if (mapGet(response, headersKey, &headersValue)) {
      if (!isObjType(headersValue, OBJ_MAP)) return false;
      *headersOut = (ObjMap*)AS_OBJ(headersValue);
    }

    return true;
  }

  return false;
}

static Value nativeHttpServe(VM* vm, int argc, Value* args) {
  if (argc < 2 || argc > 3) {
    return runtimeErrorValue(vm, "http.serve expects (port, routes[, cors]).");
  }
  int portValue = 0;
  if (!httpPortFromValue(vm, args[0], &portValue)) return NULL_VAL;
  if (!isObjType(args[1], OBJ_MAP)) {
    return runtimeErrorValue(vm, "http.serve expects (port, routes[, cors]).");
  }

  ObjMap* routes = (ObjMap*)AS_OBJ(args[1]);
  ObjMap* corsConfig = NULL;
  if (argc >= 3) {
    if (IS_NULL(args[2])) {
      corsConfig = NULL;
    } else if (isObjType(args[2], OBJ_MAP)) {
      corsConfig = (ObjMap*)AS_OBJ(args[2]);
    } else {
      return runtimeErrorValue(vm, "http.serve expects cors to be a map or null.");
    }
  }
  int requestedPort = portValue;

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  if (!httpSocketStartup()) {
    return runtimeErrorValue(vm, "http.serve failed to initialize sockets.");
  }

  ErkaoSocket server = ERKAO_INVALID_SOCKET;
  int boundPort = 0;
  bool inUse = false;
  if (!httpBindServerSocket(&server, requestedPort, &boundPort, &inUse)) {
    if (requestedPort > 0 && inUse) {
      if (!httpBindServerSocket(&server, 0, &boundPort, NULL)) {
        return runtimeErrorValue(vm, "http.serve failed to bind.");
      }
      printf("http.serve port %d in use, selected %d\n", requestedPort, boundPort);
    } else {
      return runtimeErrorValue(vm, "http.serve failed to bind.");
    }
  }

  printf("http.serve listening on http://127.0.0.1:%d\n", boundPort);
  fflush(stdout);

  bool running = true;
  while (running) {
    if (vm->instructionBudget > 0) {
      vm->instructionCount++;
      if (vm->instructionCount > vm->instructionBudget) {
        runtimeErrorValue(vm, "Instruction budget exceeded.");
        running = false;
        continue;
      }
    }
    if (!httpWaitForClient(server, HTTP_ACCEPT_TIMEOUT_MS)) {
      continue;
    }
    struct sockaddr_in clientAddr;
#ifdef _WIN32
    int addrLen = (int)sizeof(clientAddr);
#else
    socklen_t addrLen = sizeof(clientAddr);
#endif
    ErkaoSocket client = accept(server, (struct sockaddr*)&clientAddr, &addrLen);
    if (client == ERKAO_INVALID_SOCKET) {
      continue;
    }
    if (!httpSetSocketTimeouts(client, HTTP_CLIENT_TIMEOUT_MS)) {
      erkaoCloseSocket(client);
      continue;
    }

    ByteBuffer request;
    bufferInit(&request);
    size_t headerEnd = 0;
    if (!httpReadHeaders(client, &request, &headerEnd)) {
      if (request.length >= HTTP_MAX_REQUEST_BYTES) {
        (void)httpSendResponse(client, 413, "payload too large",
                               strlen("payload too large"), NULL, corsConfig);
      } else if (request.length > 0) {
        (void)httpSendResponse(client, 408, "request timeout",
                               strlen("request timeout"), NULL, corsConfig);
      }
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    const char* method = NULL;
    size_t methodLen = 0;
    const char* path = NULL;
    size_t pathLen = 0;
    if (!httpParseRequestLine(request.data, headerEnd, &method, &methodLen, &path, &pathLen)) {
      httpSendResponse(client, 400, "bad request", strlen("bad request"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    httpLogRequest(&clientAddr, path, pathLen);

    Value routeValue = NULL_VAL;
    bool found = false;
    char* methodKey = NULL;
    if (methodLen > 0 && pathLen > 0) {
      size_t methodKeyLen = methodLen + 1 + pathLen;
      methodKey = (char*)malloc(methodKeyLen + 1);
      if (!methodKey) {
        (void)httpSendResponse(client, 500, "internal error",
                               strlen("internal error"), NULL, corsConfig);
        bufferFree(&request);
        erkaoCloseSocket(client);
        continue;
      }
      memcpy(methodKey, method, methodLen);
      methodKey[methodLen] = ' ';
      memcpy(methodKey + methodLen + 1, path, pathLen);
      methodKey[methodKeyLen] = '\0';

      ObjString* routeKey = copyStringWithLength(vm, methodKey, (int)methodKeyLen);
      if (mapGet(routes, routeKey, &routeValue)) {
        found = true;
      }
    }

    if (!found) {
      ObjString* routeKey = copyStringWithLength(vm, path, (int)pathLen);
      if (mapGet(routes, routeKey, &routeValue)) {
        found = true;
      }
    }

    free(methodKey);

    if (methodLen == 7 && memcmp(method, "OPTIONS", 7) == 0) {
      httpSendResponse(client, 204, "", 0, NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    if (!found) {
      httpSendResponse(client, 404, "not found", strlen("not found"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    ObjMap* requestObj = NULL;
    bool isHandler = isObjType(routeValue, OBJ_FUNCTION) || isObjType(routeValue, OBJ_BOUND_METHOD);
    if (isHandler) {
      long contentLength = httpGetContentLength(request.data, headerEnd);
      if (contentLength > 0) {
        if (!httpReadBody(client, &request, headerEnd, contentLength)) {
          (void)httpSendResponse(client, 413, "payload too large",
                                 strlen("payload too large"), NULL, corsConfig);
          bufferFree(&request);
          erkaoCloseSocket(client);
          continue;
        }
      }
      
      ObjMap* requestHeaders = httpParseHeaders(vm, request.data, headerEnd);
      
      const char* requestBody = NULL;
      size_t requestBodyLen = 0;
      if (request.length > headerEnd) {
        requestBody = request.data + headerEnd;
        requestBodyLen = request.length - headerEnd;
      }
      
      requestObj = httpCreateRequestObject(vm, method, methodLen, path, pathLen,
                                           requestHeaders, requestBody, requestBodyLen);
    }

    int status = 200;
    const char* body = "";
    size_t bodyLen = 0;
    ObjMap* headers = NULL;
    if (!httpResponseFromValue(vm, routeValue, &status, &body, &bodyLen, &headers, requestObj)) {
      httpSendResponse(client, 500, "invalid response", strlen("invalid response"), NULL, corsConfig);
      bufferFree(&request);
      erkaoCloseSocket(client);
      continue;
    }

    if (!httpSendResponse(client, status, body, bodyLen, headers, corsConfig)) {
      (void)httpSendResponse(client, 500, "internal error", strlen("internal error"), NULL, NULL);
    }
    bufferFree(&request);
    erkaoCloseSocket(client);
    gcMaybe(vm);
  }

  erkaoCloseSocket(server);
  return NULL_VAL;
}


void stdlib_register_http(VM* vm, ObjInstance* module) {
  moduleAdd(vm, module, "get", nativeHttpGet, 1);
  moduleAdd(vm, module, "post", nativeHttpPost, 2);
  moduleAdd(vm, module, "request", nativeHttpRequest, -1);
  moduleAdd(vm, module, "serve", nativeHttpServe, -1);
}
