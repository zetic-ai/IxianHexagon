#include "server-decision.h"

#include "server-log.h"

#include <algorithm>
#include <cmath>

namespace ixsrv {

namespace {

const size_t DECISION_MAX_MEDIA = 8;

std::string text_of(const json & v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
}

void load_media_item(const json & url, const media_options & opt, decision_request & out, bool audio_ok) {
    if (!url.is_string()) {
        throw std::invalid_argument("media must be data URLs (data:image/..., data:audio/... or data:video/...;base64,...)");
    }
    const std::string s = url.get<std::string>();
    if (string_starts_with(s, "http://") || string_starts_with(s, "https://") || string_starts_with(s, "file://")) {
        throw std::invalid_argument("media must be data URLs (data:image/..., data:audio/... or data:video/...;base64,...)");
    }
    if (out.n_images + out.n_audio >= DECISION_MAX_MEDIA) {
        throw std::invalid_argument(string_format("too many images, the maximum is %zu", DECISION_MAX_MEDIA));
    }
    std::string mime;
    const raw_buffer bytes = load_media_url(s, opt, &mime);
    std::string kind = string_starts_with(mime, "image/") ? "image" : string_starts_with(mime, "audio/") ? "audio" : string_starts_with(mime, "video/") ? "video" : "";
    const std::string sniffed = sniff_media_kind(bytes.data(), bytes.size());
    if (!sniffed.empty()) kind = sniffed;
    if (kind == "video") {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "Unsupported: d1 takes no video input (images and audio only)");
    }
    if (kind == "audio" && !audio_ok) {
        throw api_error(ERROR_TYPE_NOT_SUPPORTED, "This model does not support audio input for decisions (d1-Omni-2B does)");
    }
    media_options o = opt;
    o.allow_image = true;
    o.allow_audio = audio_ok;
    for (auto & p : media_to_parts(bytes, kind, o)) {
        if (p.type == zx::Part::Type::Image) out.n_images++;
        else out.n_audio++;
        out.media.push_back(std::move(p));
    }
}

}  // namespace

decision_request parse_decision_request(const json & body, const media_options & media) {
    decision_request out;

    if (!body.is_object()) {
        throw std::invalid_argument("the request body must be a JSON object");
    }
    if (!body.contains("state")) {
        throw std::invalid_argument("\"state\" must be provided");
    }
    if (!body.contains("questions") || !body.at("questions").is_object() || body.at("questions").empty()) {
        throw std::invalid_argument("\"questions\" must be a non-empty object");
    }
    if (body.contains("videos") && !body.at("videos").is_null() && !body.at("videos").empty()) {
        throw std::invalid_argument("\"videos\" is not supported");
    }

    // media: "images" is an alias of "files"
    for (const char * key : {"files", "images"}) {
        if (!body.contains(key) || body.at(key).is_null()) continue;
        if (!body.at(key).is_array()) {
            throw std::invalid_argument(string_format("\"%s\" must be an array", key));
        }
        for (const auto & url : body.at(key)) {
            load_media_item(url, media, out, media.allow_audio);
        }
    }

    // the state: text, a JSON document, null, or chat messages whose media parts are taken out
    const json & state = body.at("state");
    const bool is_wrapped = state.is_object() && state.contains("messages");
    const json & messages = is_wrapped ? state.at("messages") : state;
    if (messages.is_array()) {
        json messages_out = json::array();
        for (const auto & msg : messages) {
            if (!msg.is_object() || !msg.contains("content") || !msg.at("content").is_array()) {
                messages_out.push_back(msg);
                continue;
            }
            json content = json::array();
            for (const auto & part : msg.at("content")) {
                const std::string type = part.is_object() ? json_value(part, "type", std::string()) : "";
                if (type == "image_url" && part.contains("image_url")) {
                    const json & iu = part.at("image_url");
                    load_media_item(iu.is_object() && iu.contains("url") ? iu.at("url") : iu, media, out, media.allow_audio);
                } else if (type == "input_audio" && part.contains("input_audio")) {
                    const json ia = json_value(part, "input_audio", json::object());
                    load_media_item(ia.contains("data") ? ia.at("data") : json_value(ia, "url", json()), media, out, media.allow_audio);
                } else if (type == "video_url" || type == "input_video") {
                    throw api_error(ERROR_TYPE_NOT_SUPPORTED, "Unsupported: d1 takes no video input (images and audio only)");
                } else {
                    content.push_back(part);
                }
            }
            json msg_out = msg;
            msg_out["content"] = content;
            messages_out.push_back(msg_out);
        }
        json state_out = state;
        if (is_wrapped) {
            state_out["messages"] = messages_out;
        } else {
            state_out = messages_out;
        }
        out.state = zx::State::Json(state_out.dump());
    } else if (state.is_null()) {
        out.state = zx::State::None();
    } else if (state.is_string()) {
        out.state = zx::State::Text(state.get<std::string>());
    } else {
        out.state = zx::State::Json(state.dump());
    }
    if (state.is_null() && out.media.empty()) {
        throw std::invalid_argument("\"state\" must be provided (a null state needs media)");
    }

    // questions, in request order
    for (const auto & [id, q] : body.at("questions").items()) {
        auto err = [&id](const std::string & msg) { return std::invalid_argument("questions." + id + ": " + msg); };
        if (!q.is_object()) {
            throw err("must be an object");
        }
        if (!q.contains("instructions") || q.at("instructions").is_null()) {
            throw err("\"instructions\" must be provided");
        }
        const std::string instructions = text_of(q.at("instructions"));
        if (instructions.empty()) {
            throw err("\"instructions\" must be non-empty text, an object or an array");
        }
        const std::string type_name = json_value(q, "type", std::string());
        const json criteria = q.contains("criteria") ? q.at("criteria") : json();

        decision_question_meta meta;
        meta.id = id;
        zx::Question question;

        if (type_name == "choice") {
            if (!criteria.is_object() || criteria.empty()) {
                throw err("\"criteria\" must be a non-empty object");
            }
            std::vector<std::pair<std::string, std::string>> options;
            for (const auto & [key, description] : criteria.items()) {
                if (key.empty()) {
                    throw err("option keys must be non-empty");
                }
                options.emplace_back(key, text_of(description));
                meta.option_keys.push_back(key);
                meta.option_descriptions.push_back(description);
            }
            question = zx::Question::Choice(instructions, options, json_value(q, "multiple", false));
            meta.type = zx::Question::Type::Choice;
        } else if (type_name == "score") {
            if (!criteria.is_array() || criteria.size() < 2 || criteria.size() > 10) {
                throw err("\"criteria\" must be an array of 2 to 10 levels");
            }
            std::vector<std::string> levels;
            for (size_t i = 0; i < criteria.size(); i++) {
                levels.push_back(text_of(criteria.at(i)));
                meta.option_keys.push_back(std::to_string(i));
                meta.option_descriptions.push_back(criteria.at(i));
            }
            question = zx::Question::Score(instructions, levels);
            meta.type = zx::Question::Type::Score;
        } else if (type_name == "noul") {
            if (!criteria.is_null() && !criteria.is_object()) {
                throw err("\"criteria\" must be an object");
            }
            question = zx::Question::Noul(instructions);
            if (criteria.is_object()) {
                for (const auto & [key, description] : criteria.items()) {
                    if (key != "false" && key != "true" && key != "yes" && key != "no") {
                        throw err("\"criteria\" may only describe \"false\" and \"true\"");
                    }
                    question.criteria.emplace_back(key == "yes" ? "true" : key == "no" ? "false" : key, text_of(description));
                }
            }
            meta.option_keys = {"true", "false"};
            meta.option_descriptions = {json(), json()};
            meta.type = zx::Question::Type::Noul;
        } else {
            throw err("\"type\" must be one of: choice, score, noul");
        }
        out.questions.emplace_back(id, std::move(question));
        out.meta.push_back(std::move(meta));
    }
    return out;
}

json format_decision_answer(const decision_question_meta & meta, const zx::Answer & a) {
    json answer = json {{"type", meta.type == zx::Question::Type::Noul ? "noul" : meta.type == zx::Question::Type::Choice ? "choice" : "score"}};

    if (meta.type == zx::Question::Type::Noul) {
        answer["noul"] = a.noul;
        return answer;
    }

    // probabilities keyed by the request's option keys (choice: criteria keys; score: "0".."n-1")
    json probabilities = json::object();
    const size_t n = meta.option_keys.size();
    double max_prob = 0.0;
    for (size_t i = 0; i < n; i++) {
        double p = 0.0;
        if (i < a.probs.size()) {
            p = a.probs[i];
        } else if (!a.labels.empty()) {
            // the SDK's labels may be ordered differently: match by label text
            for (size_t j = 0; j < a.labels.size() && j < a.probs.size(); j++) {
                if (a.labels[j] == meta.option_keys[i]) p = a.probs[j];
            }
        }
        probabilities[meta.option_keys[i]] = p;
        max_prob = std::max(max_prob, p);
    }
    const double confidence = a.confidence > 0 ? a.confidence : max_prob;

    if (meta.type == zx::Question::Type::Choice) {
        answer["choice"] = a.choice;
        if (!a.choices.empty()) {
            answer["choices"] = a.choices;
        }
        answer["probabilities"] = probabilities;
        answer["confidence"] = confidence;
        return answer;
    }

    json legend = json::object();
    for (size_t i = 0; i < n; i++) {
        legend[meta.option_keys[i]] = meta.option_descriptions[i];
    }
    answer["score"] = (double) a.score;
    answer["legend"] = legend;
    answer["probabilities"] = probabilities;
    answer["confidence"] = confidence;
    return answer;
}

}  // namespace ixsrv
