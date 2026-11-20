// Ixian Hexagon server - POST /v1/systemone: typed decisions with the d1 models (Decision::systemOne).
// Request and answer shapes follow llama.cpp's TypeSafe-compatible System One API (server-decision.cpp, lfm3-d1).
#pragma once

#include "server-chat.h"
#include "server-engine.h"
#include "server-utils.h"

#include <string>
#include <vector>

namespace ixsrv {

struct decision_question_meta {
    std::string id;
    zx::Question::Type type;
    std::vector<std::string> option_keys;         // choice: the criteria keys; score: "0".."n-1"; noul: {"true","false"}
    std::vector<json>        option_descriptions; // the criteria values as given (null when absent)
};

struct decision_request {
    zx::State state;
    std::vector<std::pair<std::string, zx::Question>> questions;
    std::vector<decision_question_meta> meta;
    std::vector<zx::Part> media;
    size_t n_images = 0;
    size_t n_audio  = 0;
};

// body -> SDK request; validation errors are std::invalid_argument (400)
decision_request parse_decision_request(const json & body, const media_options & media);

// one answer in the API shape: {"type": "noul", "noul": p} / {"type": "choice", "choice", "probabilities", "confidence"} /
// {"type": "score", "score", "legend", "probabilities", "confidence"}
json format_decision_answer(const decision_question_meta & meta, const zx::Answer & answer);

}  // namespace ixsrv
