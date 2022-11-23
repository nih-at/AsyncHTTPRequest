//
// Created by Dieter Baron on 22/03/24.
//

#ifndef ASYNCHTTPREQUEST_ASYNCHTTPREQUEST_H
#define ASYNCHTTPREQUEST_ASYNCHTTPREQUEST_H

#include <functional>
#include <string>

#include <Arduino.h>
#ifdef USE_SSL
#include <AsyncTCP_SSL.hpp>
#else
#include <AsyncTCP.h>
typedef AsyncClient AsyncSSLClient;
#endif

#define HTTP_BUFFER_FRAGMENT_SIZE 512

class AsyncHTTPRequest {
public:
    enum Error {
        ERROR_OK,
        ERROR_SCHEME,
        ERROR_IN_USE,
        ERROR_CANNOT_CONNECT,
        ERROR_TIMEOUT,
        ERROR_CONNECTION_CLOSED
    };

    class Buffer: public Print {
    public:
        Buffer() = default;
        Buffer(const char* data, size_t length) { write(data, length); }
        size_t write(uint8_t c) { write(reinterpret_cast<const char*>(&c), 1); return 1; }
        void write(const char* data, size_t length);
        size_t read(char* data, size_t length);
        char* readline(char* data, size_t length);
        void print(const char* string) { write(string, strlen(string)); }
        void print(String string) { write(string.c_str(), string.length()); }
        void consume(size_t length) { read(nullptr, length); }

        size_t available() { return end - start; }
        void clear();

        // Gets up to length bytes without consuming them, sets length to number of bytes returned.
        // (This only returns data from a single fragment.)
        const char* get(size_t* length);

    private:
        struct Fragment {
            char data[HTTP_BUFFER_FRAGMENT_SIZE];
            Fragment *next = nullptr;
        };
        size_t start = 0;
        size_t end = 0;
        Fragment *first = nullptr;
        Fragment *last = nullptr;
    };

    typedef std::function<void(AsyncHTTPRequest* request)> CompletionHandler;
    typedef std::function<void(AsyncHTTPRequest* request)> DataHandler;
    typedef std::function<void(AsyncHTTPRequest* request, Error error)> ErrorHandler;

    AsyncHTTPRequest() = default;
    ~AsyncHTTPRequest();

    Error send(const char* method, const char* url, const char* content_type, Buffer* body);
    Error get(const char* url) { send("GET", url, nullptr, nullptr); }
    Error post(const char* url, const char* content_type, Buffer* body) { send("POST", url, content_type, body); }

    void abort();

    // These handlers will be called on a background thread.
    void onCompletion(CompletionHandler handler) {completionHandler = handler; }
    void onError(ErrorHandler handler) { errorHandler = handler; }
    void onReceivedData(DataHandler handler) { receivedDataHandler = handler; }

    bool isComplete() const { return state == ERROR || state == COMPLETE; }
    int status() const { return httpStatus; }
    const char* contentType() const { return state > RECEIVING_HEADERS ? response_content_type.c_str() : nullptr; }
    size_t contentLength() const;
    Error error() const { return current_error; }
    const char* errorString() const { return lastErrorString.c_str(); }
    size_t read(char* data, size_t length);

private:
    enum State {
        EMPTY,
        ERROR,
        CONNECTING,
        SENDING_REQUEST,
        SENDING_BODY,
        RECEIVING_STATUS_LINE,
        RECEIVING_HEADERS,
        RECEIVING_BODY,
        COMPLETE
    };

    class URL {
    public:
        URL(const char* url);

        std::string scheme;
        std::string host;
        int port = 0;
        std::string path;
    };

    class Lock {
    public:
        Lock(SemaphoreHandle_t mutex);
        ~Lock();

        void unlock();

    private:
        SemaphoreHandle_t mutex;
        bool locked;
    };

    CompletionHandler completionHandler = nullptr;
    ErrorHandler errorHandler = nullptr;
    DataHandler receivedDataHandler = nullptr;

    SemaphoreHandle_t mutex = nullptr;

    State state = EMPTY;
    Error current_error = ERROR_OK;
    int error_code = 0;
    std::string lastErrorString;
    AsyncSSLClient* client = nullptr;

    Buffer buffer;
    Buffer* requestBody = nullptr;

    int httpStatus = 0;
    String responseContentType;
    bool chunkedResponse = false;
    size_t chunkSize = 0;
    bool inChunkSize = false;
    size_t responseContentLength = 0;
    size_t dataReceived = 0;
    bool haveContentLength = false;
    Buffer* responseBody = nullptr;

    std::string response_content_type;

    bool notify_data = false;
    bool notify_complete = false;
    bool notify_error = false;


    void parseHeader(const char* line);
    static size_t parseInteger(const char* string);
    void parseStatusLine(const char* line);
    void processBodyData(char* data, size_t length);
    void processChunkedBodyData(char* data, size_t length);
    void requestCompleted();
    void sendData();
    bool sendData(Buffer* data);

    void close_client();

    void handleAck(size_t len, uint32_t time);
    void handleConnect();
    void handleData(char *data, size_t length);
    void handleDisconnect();
    void handleError(int error);
    void handleError(Error new_error, const char* detail = nullptr);
    void handleTimeout(int timeout);

    void post_notifications();
};


#endif //ASYNCHTTPREQUEST_ASYNCHTTPREQUEST_H
