//
// Created by benny on 4/26/26.
//

#include "tracker_handlers.hpp"
#include "token_store.hpp"
#include "util/constants.hpp"
#include "util/json_parser.hpp"
#include <httplib.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

void log(const std::string& verb, const std::string& token,
         const std::string& detail) {
    std::cout << "[tracker] " << verb << " "
              << token.substr(0, 4) << "..." << detail << "\n";
}

} // namespace

bool valid_token(const std::string& token) {
    if (token.size() != TOKEN_SIZE) { return false; }
    return std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); // hex char
    });
}

std::string client_ip(const httplib::Request& req) {
    if (req.has_header("X-Forwarded-For")) {
        const std::string xff = req.get_header_value("X-Forwarded-For");
        const size_t comma_pos = xff.find(",");
        return comma_pos != std::string::npos ? xff.substr(0, comma_pos) : "";
    }
    return req.remote_addr;
}

void handle_register(const httplib::Request& req,
                     httplib::Response& res,
                     TokenStore& store) {
    const std::string token = json_get_str(req.body, "token");
    const uint16_t port = json_get_u16(req.body, "sender_port");
    const std::string sender_ip = json_get_str(req.body, "sender_ip");

    if (!valid_token(token) || port == 0) {
        res.status = 400;
        res.set_content("{\"error\":\"malformed token or missing port\"}", "application/json");
        return;
    }

    const std::string ip = sender_ip.empty() ? client_ip(req) : sender_ip;
    store.upsert(token, Record{ip, port, {}});

    res.status = 200;
    res.set_content("{\"sender_ip\":\"" + ip + "\"}", "application/json");
    log("register", token, "ip=" + ip + ", port=" + std::to_string(port));
}

void handle_resolve(const httplib::Request& req,
                    httplib::Response& res,
                    TokenStore& store) {
    const std::string token = req.path_params.at("token");
    if (!valid_token(token)) {
        res.status = 400;
        res.set_content("{\"error\":\"malformed token\"}", "application/json");
        return;
    }

    const auto record = store.resolve(token);
    if (!record) {
        res.status = 404;
        res.set_content("{\"error\":\"token not found\"}", "application/json");
        return;
    }

    res.status = 200;
    res.set_content("{\"sender_ip\":\"" + record->sender_ip + "\","
                    "\"sender_port\":" + std::to_string(record->sender_port) + "}", "application/json");
    log("resolve", token, "-> " + record->sender_ip + ":" + std::to_string(record->sender_port));
}

// v0.6: any caller can unregister
// v0.7+: match on (token, sender_ip)
void handle_unregister(const httplib::Request& req,
                       httplib::Response& res,
                       TokenStore& store) {
    const std::string token = req.path_params.at("token");
    if (!valid_token(token)) {
        res.status = 400;
        res.set_content("{\"error\":\"malformed token\"}", "application/json");
        return;
    }

    store.remove(token);
    res.status = 204;
    log("unregister", token, "");
}