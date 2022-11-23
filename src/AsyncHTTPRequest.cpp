//
// Created by Dieter Baron on 22/03/24.
//

#include "AsyncHTTPRequest.h"

#ifdef USE_SSL
#include <AsyncTCP_SSL.h>
#endif

#define DEBUG_HTTP

#ifdef DEBUG_HTTP
#define DEBUG(x) Serial.println(String("HTTP: ") + x)
#else
#define DEBUG(x) ((void)0)
#endif

#define HTTP_MAX_LINE_LENGTH 512

AsyncHTTPRequest::~AsyncHTTPRequest() {
    close_client();
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
    delete requestBody;
    delete responseBody;
}

void AsyncHTTPRequest::abort() {
    auto lock = Lock(mutex);

    // TODO: implement
}


size_t AsyncHTTPRequest::contentLength() const {
    if (haveContentLength){
        return responseContentLength;
    }
    else if (state == COMPLETE) {
        return dataReceived;
    }
    else {
        return 0;
    }
}


size_t AsyncHTTPRequest::read(char* data, size_t length) {
    auto lock = Lock(mutex);

    if (responseBody) {
        return responseBody->read(data, length);
    }
    else {
        return 0;
    }
}



AsyncHTTPRequest::Error AsyncHTTPRequest::send(const char* method, const char* url_string, const char* content_type, Buffer* body) {
    if (state != EMPTY) {
        handleError(ERROR_IN_USE);
        return error();
    }

    auto url = URL(url_string);

    auto use_ssl = false;

    if (url.scheme == "http") {
    }
#ifdef USE_SSL
    else if (url.scheme == "https") {
        use_ssl = true;
    }
#endif
    else {
        handleError(ERROR_SCHEME, url.scheme.c_str());
        return error();
    }

    mutex = xSemaphoreCreateMutex();
    client = new AsyncSSLClient();
    client->onAck([this](void *arg, AsyncSSLClient *client, size_t len, uint32_t time) {
                this->handleAck(len, time);
    });
    client->onConnect([this](void *arg, AsyncSSLClient *client) {
        this->handleConnect();
    });
    client->onData([this](void *arg, AsyncSSLClient *client, void *data, size_t length) {
        this->handleData((char *) data, length);
    });
    client->onDisconnect([this](void *arg, AsyncSSLClient *client) {
        this->handleDisconnect();
    });
    client->onError([this](void *arg, AsyncSSLClient *client, int error) {
        this->handleError(error);
    });
    client->onTimeout([this](void *arg, AsyncSSLClient *client, int timeout) {
        this->handleTimeout(timeout);
    });

    buffer.print(method);
    buffer.print (" ");
    buffer.print(url.path.c_str());
    buffer.print(" HTTP/1.1\r\nHost: ");
    buffer.print(url.host.c_str());
    buffer.print("\r\n");
    if (body != nullptr) {
        if (content_type != nullptr) {
            buffer.print("Content-Type: ");
            buffer.print(content_type);
            buffer.print("\r\n");
        }
        buffer.print("Content-Length: ");
        buffer.print(String(body->available()));
        buffer.print("\r\n");
    }
    buffer.print("\r\n");

    DEBUG("Request size: " + buffer.available());

    requestBody = body;

    state = CONNECTING;
    DEBUG("Connecting");
    if (!client->connect(url.host.c_str(), url.port
#ifdef USE_SSL
                         , use_ssl
#endif
                         )) {
        handleError(ERROR_CANNOT_CONNECT);
        buffer.clear();
        requestBody = nullptr;
        delete client;
        client = nullptr;
        return error();
    }

    return ERROR_OK;
}


void AsyncHTTPRequest::handleAck(size_t len, uint32_t time) {
    DEBUG("Got TCP ACK.");
    auto lock = Lock(mutex);

    if (state == CONNECTING) {
        state = SENDING_REQUEST;
    }
    (void)len;
    (void)time;
    sendData();

    lock.unlock();
    post_notifications();
}


void AsyncHTTPRequest::handleConnect() {
    DEBUG("Got TCP Connected.");
    auto lock = Lock(mutex);

    state = SENDING_REQUEST;
    sendData();

    lock.unlock();
    post_notifications();
}


void AsyncHTTPRequest::handleData(char* data, size_t length) {
    DEBUG("Got TCP Data (" + length + " bytes).");

    auto lock = Lock(mutex);

    switch (state) {
        case RECEIVING_STATUS_LINE:
        case RECEIVING_HEADERS: {
            buffer.write(data, length);

            while (state == RECEIVING_STATUS_LINE || state == RECEIVING_HEADERS) {
                char line_buffer[HTTP_MAX_LINE_LENGTH];
                auto line = buffer.readline(line_buffer, sizeof(line_buffer));
                if (line == nullptr) {
                    break;
                }
                //DEBUG("Got line: '" + line + "'");
                if (state == RECEIVING_STATUS_LINE) {
                    parseStatusLine(line);
                } else {
                    parseHeader(line);
                }
            }
            return;
        }

        case RECEIVING_BODY:
            processBodyData(data, length);
            break;

        default:
            // TODO: how to handle?
            break;
    }

    lock.unlock();
    post_notifications();
}


void AsyncHTTPRequest::handleDisconnect() {
    DEBUG("Got TCP Disconnected.");
    auto lock = Lock(mutex);

    switch (state) {
        case RECEIVING_BODY:
            if (!chunkedResponse && !haveContentLength) {
                DEBUG("Request completed by disconnect");
                requestCompleted();
                break;
            }
            // fallthrough
        case CONNECTING:
        case SENDING_REQUEST:
        case SENDING_BODY:
        case RECEIVING_STATUS_LINE:
        case RECEIVING_HEADERS:
            DEBUG("Server closed connection prematurely");
            handleError(ERROR_CONNECTION_CLOSED);
            break;

        case EMPTY:
        case ERROR:
        case COMPLETE:
            break;
    }
    delete client;
    client = nullptr;

    lock.unlock();
    post_notifications();
}

void AsyncHTTPRequest::handleError(int error_code) {
    DEBUG("Got TCP Error" + error_code);
    auto lock = Lock(mutex);

    if (state == CONNECTING) {
        handleError(ERROR_CANNOT_CONNECT, client->errorToString(error_code));
    }
    else {
        handleError(ERROR_CONNECTION_CLOSED, client->errorToString(error_code));
    }
    delete client;
    client = nullptr;

    lock.unlock();
    post_notifications();
}


void AsyncHTTPRequest::handleError(Error new_error, const char* detail) {
    if (state != ERROR) {
        bool call_handler = state == EMPTY;
        current_error = new_error;
        state = ERROR;

        switch (error()) {
            case ERROR_OK:
                last_error_string = "No error";
                break;
            case ERROR_SCHEME:
                last_error_string = "Unsupported URL scheme";
                break;
            case ERROR_IN_USE:
                last_error_string = "Request already started";
                break;
            case ERROR_CANNOT_CONNECT:
                last_error_string = "Cannot connect";
                break;
            case ERROR_TIMEOUT:
                last_error_string = "Request timed out";
                break;
            case ERROR_CONNECTION_CLOSED:
                last_error_string = "Server closed connection";
                break;
        }
        if (detail) {
            last_error_string += ": ";
            last_error_string += detail;
        }

        DEBUG("Error: " + last_error_string.c_str());

        if (call_handler) {
            notify_error = true;
        }
    }
}


void AsyncHTTPRequest::handleTimeout(int timeout) {
    auto lock = Lock(mutex);

    DEBUG("Timeout");
    (void)timeout;
    handleError(ERROR_TIMEOUT);
    delete client;
    client = nullptr;

    lock.unlock();
    post_notifications();
}


void AsyncHTTPRequest::parseHeader(const char *line) {
    if (line[0] == '\0') {
        DEBUG("End of headers");
        state = RECEIVING_BODY;
        char data[HTTP_BUFFER_FRAGMENT_SIZE];
        size_t length;
        while ((length = buffer.read(data, sizeof(data))) > 0) {
            processBodyData(data, length);
        }
        return;
    }

    auto value = strchr(line, ':');
    if (value == nullptr) {
        // TODO: invalid header
        return;
    }
    *value = '\0';
    value += 1;
    value += strspn(value, " \t");

    if (strcasecmp(line, "Content-Length") == 0) {
        responseContentLength = parseInteger(value);
        DEBUG("Got Content-Length " + responseContentLength);
        haveContentLength = true;
    }
    else if (strcasecmp(line, "Content-Type") == 0) {
        response_content_type = value;
        DEBUG("Got Content-Type '" + response_content_type.c_str() + "'");
    }
    else if (strcasecmp(line, "Transfer-Encoding") == 0) {
        if (strcasecmp(value, "chunked") == 0) {
            chunkedResponse = true;
            inChunkSize = true;
            chunkSize = 0;
            DEBUG("Got chunked response");
        }
    }
}


size_t AsyncHTTPRequest::parseInteger(const char *string) {
    size_t value = 0;

    for (auto i = 0; isdigit(string[i]); i++) {
        value = value * 10 + (string[i] - '0');
    }

    return value;
}

void AsyncHTTPRequest::parseStatusLine(const char *line) {
    line = strchr(line, ' ');
    if (line == nullptr) {
        // TODO: handle invalid HTTP line
        return;
    }
    line += strspn(line, " ");
    httpStatus = parseInteger(line);
    DEBUG("Got HTTP status " + httpStatus);
    state = RECEIVING_HEADERS;
}


void AsyncHTTPRequest::processBodyData(char* data, size_t length) {
    DEBUG("Got body data (" + length + ")");
#ifdef DEBUG_HTTP_FULL
    Serial.write(data, length);
#endif
    if (haveContentLength && dataReceived + length > responseContentLength) {
        length = responseContentLength - dataReceived;
    }

    if (responseBody == nullptr) {
        responseBody = new Buffer();
    }
    DEBUG("Writing " + length + " bytes to reqeustBody buffer");
    responseBody->write(data, length);
    dataReceived += length;

    notify_data = true;

    if (haveContentLength && dataReceived >= responseContentLength) {
        requestCompleted();
    }
}


void AsyncHTTPRequest::processChunkedBodyData(char* data, size_t length) {
    while (length > 0) {
        if (inChunkSize) {
            auto i = 0;
            while (i < length && isdigit(data[i])) {
                chunkSize = chunkSize * 10 + (data[i] - '0');
                i += 1;
            }
            while (i < length) {
                if (data[i] == '\r') {
                    continue;
                } else if (data[i] == '\n') {
                    inChunkSize = false;
                    if (chunkSize == 0) {
                        requestCompleted();
                        return;
                    }
                    data += i;
                    length -= i;
                } else {
                    // TODO: invalid character in chunk size
                }
                i += 1;
            }
        }
        else {
            auto data_length = length < chunkSize ? length : chunkSize;
            processBodyData(data, data_length);
            data += data_length;
            length -= data_length;
            chunkSize -= data_length;
            if (chunkSize == 0) {
                inChunkSize = true;
            }
        }
    }
}


void AsyncHTTPRequest::requestCompleted() {
    DEBUG("Request complete");
    state = COMPLETE;
    notify_complete = true;
}


void AsyncHTTPRequest::sendData() {
    if (state == SENDING_REQUEST) {
        if (sendData(&buffer)) {
            if (requestBody != nullptr) {
                DEBUG("Sending body");
                state = SENDING_BODY;
            }
            else {
                DEBUG("Receiving response");
                state = RECEIVING_STATUS_LINE;
            }
        }
    }

    if (state == SENDING_BODY) {
        if (sendData(requestBody)) {
            DEBUG("Receiving response");
            state = RECEIVING_STATUS_LINE;
        }
    }
}


bool AsyncHTTPRequest::sendData(Buffer* buffer) {
#if 0
    if (!client->canSend()) {
        DEBUG("Can't send data");
        return false;
    }
#endif
    size_t to_send = client->space();

    DEBUG("Sending " + to_send + " bytes");

    while (to_send > 0) {
        auto length = to_send;
        auto data = buffer->get(&length);
        if (data == nullptr) {
            return true;
        }
#ifdef DEBUG_HTTP_FULL
        Serial.write(data, length);
#endif
        length = client->add(data, length);
        if (length == 0) {
            return false;
        }
        buffer->consume(length);
        to_send -= length;
    }

    return false;
}


void AsyncHTTPRequest::Buffer::clear() {
    while (first != nullptr) {
        auto next = first->next;
        delete first;
        first = next;
    }
    last = nullptr;
    start = end = 0;
}


const char* AsyncHTTPRequest::Buffer::get(size_t* length) {
    size_t offset = start % HTTP_BUFFER_FRAGMENT_SIZE;

    if (*length > available()) {
        *length = available();
    }
    if (*length > HTTP_BUFFER_FRAGMENT_SIZE - offset) {
        *length = HTTP_BUFFER_FRAGMENT_SIZE - offset;
    }

    if (*length == 0) {
        return nullptr;
    }
    return first->data + offset;
}


size_t AsyncHTTPRequest::Buffer::read(char* data, size_t length) {
    size_t bytes_read = 0;

    if (length > available()) {
        length = available();
    }

    while (bytes_read < length) {
        auto offset = start % HTTP_BUFFER_FRAGMENT_SIZE;
        auto left = HTTP_BUFFER_FRAGMENT_SIZE - offset;
        if (left > (length - bytes_read)) {
            left = (length - bytes_read);
        }
        if (data != nullptr) {
            memcpy(data + bytes_read, first->data + offset, left);
        }

        start += left;
        bytes_read += left;
        if ((start % HTTP_BUFFER_FRAGMENT_SIZE) == 0) {
            auto fragment = first;
            first = first->next;
            delete fragment;
        }
    }

    if (start == end) {
        if (first != nullptr) {
            delete first;
        }
        first = last = nullptr;
        start = end = 0;
    }

    return bytes_read;
}


char* AsyncHTTPRequest::Buffer::readline(char *data, size_t length) {
    size_t n = 0;
    size_t i = start % HTTP_BUFFER_FRAGMENT_SIZE;
    auto fragment = first;
    bool cr = false;

    while (fragment || i < (end % HTTP_BUFFER_FRAGMENT_SIZE)) {
        n += 1;
        if (fragment->data[i] == '\r') {
            cr = true;
        }
        else if (fragment->data[i] == '\n') {
            if (n > length) {
                // TODO: handle too long line
                n = length;
                cr = false;
            }
            read(data, n);
            data[n - (cr ? 2 : 1)] = '\0';
            return data;
        }
        else {
            cr = false;
        }
        i += 1;
        if (i >= HTTP_BUFFER_FRAGMENT_SIZE) {
            fragment = fragment->next;
            i = 0;
        }
    }

    return NULL;
}


void AsyncHTTPRequest::Buffer::write(const char* data, size_t length) {
    while (length > 0) {
        if (first == nullptr) {
            first = new Fragment();
            last = first;
        }
        else if ((end % HTTP_BUFFER_FRAGMENT_SIZE) == 0) {
            last->next = new Fragment();
            last = last->next;
        }
        auto offset = end % HTTP_BUFFER_FRAGMENT_SIZE;
        auto to_copy = HTTP_BUFFER_FRAGMENT_SIZE - offset;
        if (to_copy > length) {
            to_copy = length;
        }
        memcpy(last->data + offset, data, to_copy);
        data += to_copy;
        length -= to_copy;
        end += to_copy;
    }
}


AsyncHTTPRequest::URL::URL(const char *url) {
    const char *colon = strchr(url, ':');

    if (colon == nullptr) {
        scheme = url;
        return;
    }

    scheme = std::string(url, colon - url);

    auto rest = colon + 1;

    if (rest[0] != '/' || rest[1] != '/') {
        path = rest;
        return;
    }
    else {
        rest += 2;
    }

    const char* slash = strchr(rest, '/');
    if (slash == nullptr) {
        slash = rest + strlen(rest);
    }
    colon = strchr(rest, ':');
    if (colon != nullptr && colon < slash) {
        port = parseInteger(colon + 1);
    }
    else {
        colon = slash;
        if (scheme == "http") {
            port = 80;
        }
        else if (scheme == "https") {
            port = 443;
        }
    }

    host = std::string(rest, colon - rest);
    path = slash;
}


void AsyncHTTPRequest::close_client() {
    if (client != nullptr) {
        client->close();
        delete client;
        client = nullptr;
    }
}


void AsyncHTTPRequest::post_notifications() {
    if (notify_error) {
        if (errorHandler != nullptr) {
            DEBUG("Posting error notification.");
            errorHandler(this, error());
        }
        notify_error = false;
        notify_data = false;
        notify_complete = false;
        close_client();
        return;
    }

    if (notify_data) {
        if (receivedDataHandler != nullptr) {
            DEBUG("Posting data notification.");
            receivedDataHandler(this);
        }
        notify_data = false;
    }

    if (notify_complete) {
        if (completionHandler != nullptr) {
            DEBUG("Posting completion notification.");
            completionHandler(this);
        }
        notify_complete = false;
        close_client();
    }
}


AsyncHTTPRequest::Lock::Lock(SemaphoreHandle_t mutex): mutex(mutex) {
    DEBUG("Locking mutex.");
    xSemaphoreTake(mutex, portMAX_DELAY);
    locked = true;
    DEBUG("Mutex locked.");
}

void AsyncHTTPRequest::Lock::unlock() {
    if (locked) {
        DEBUG("Unlocking mutex.");
        xSemaphoreGive(mutex);
        locked = false;
    }
}

AsyncHTTPRequest::Lock::~Lock() {
    unlock();
}
