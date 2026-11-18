cmake_minimum_required(VERSION 3.16)
project(mailserver VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql)

# --- httplib (header-only) ---
# Download and use cpp-httplib for HTTP API server
# Clone: git clone https://github.com/yhirose/cpp-httplib.git
# Or set CPHTTPLIB_DIR to the path where httplib.h lives.
set(CPHTTPLIB_DIR "" CACHE PATH "Path to cpp-httplib header directory")
if(NOT CPHTTPLIB_DIR)
    message(FATAL_ERROR "Please set CPHTTPLIB_DIR to the directory containing httplib.h")
endif()

# --- Sources ---
set(SOURCES
    main.cpp
    src/ConfigManager.cpp
    src/Logger.cpp
    src/auth/AuthManager.cpp
    src/auth/UserRepository.cpp
    src/tls/TlsContext.cpp
    src/storage/MaildirStorage.cpp
    src/queue/QueueManager.cpp
    src/queue/OutboundWorker.cpp
    src/filters/SpamFilter.cpp
    src/parser/MimeParser.cpp
    src/api/ApiServer.cpp
    src/api/WebmailApi.cpp
    src/systemd/ServiceNotifier.cpp
    src/services/SmtpServer.cpp
    src/services/SmtpConnection.cpp
    src/services/ImapServer.cpp
    src/services/ImapConnection.cpp
    src/services/ImapMime.cpp
    src/services/Pop3Server.cpp
    src/services/Pop3Connection.cpp
)

set(HEADERS
    src/ConfigManager.h
    src/Logger.h
    src/auth/AuthManager.h
    src/auth/UserRepository.h
    src/tls/TlsContext.h
    src/storage/MaildirStorage.h
    src/queue/QueueManager.h
    src/queue/OutboundWorker.h
    src/filters/SpamFilter.h
    src/parser/MimeParser.h
    src/api/ApiServer.h
    src/api/WebmailApi.h
    src/systemd/ServiceNotifier.h
    src/utils/RateLimiter.h
    src/services/SmtpServer.h
    src/services/SmtpConnection.h
    src/services/ImapServer.h
    src/services/ImapConnection.h
    src/services/ImapMime.h
    src/services/Pop3Server.h
    src/services/Pop3Connection.h
)

add_executable(mailserver ${SOURCES} ${HEADERS})

target_include_directories(mailserver PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/api
    ${CPHTTPLIB_DIR}
)

target_link_libraries(mailserver PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    crypt
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Install
install(TARGETS mailserver RUNTIME DESTINATION bin)
install(FILES config.json DESTINATION /etc/mailserver/)
