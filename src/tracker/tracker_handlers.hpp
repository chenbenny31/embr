//
// Created by benny on 4/26/26.
//

#pragma once
#include "token_store.hpp"
#include <httplib.h>

// Validate token shape: 16 lowercase hex chars
bool valid_token(const std::string& token);

// Infer client IP from x-forward-for or req.remote_addr
std::string client_ip(const httplib::Request& req);

// POST /register
// body: {"token":"16hex","port":int}
// 200 -> record
// 400 on malformed token
void handle_register(const httplib::Request& req,
                      httplib::Response& res,
                      TokenStore& store);

// GET /resolve/:token
// 200 -> resolved record JSON
// 404 if no such token
// 400 on malformed token
void handle_resolve(const httplib::Request& req,
                    httplib::Response& res,
                    TokenStore& store);

// POST /unregister/:token
// 200 (idempotent)
// 400 on malformed token
void handle_unregister(const httplib::Request& req,
                       httplib::Response& res,
                       TokenStore& store);