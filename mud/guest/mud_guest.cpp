/*
 * mud_guest.cpp -- the Middleham MUD state machine, ported from
 * CrucibleBench_Phase1's Source_Code/state_machine.py and
 * classifier.py's _fallback_classification(), running inside the
 * libriscv sandbox (rvlinux) per the sandboxed-godot-in-zone-server-h2o
 * decision doc (multiplayer-fabric-manuals PR #128). This guest runs
 * offline: no sockets, no filesystem. Two vmcall()-reachable entry
 * points, mud_boot()/mud_step(), match that decision doc's own
 * godot_boot()/godot_tick() naming.
 *
 * Deviation from the Python original, noted for the differential test
 * in the plan's step 7: room-item shuffling and NPC trust/suspicion
 * jitter use a ChaCha20-keystream PRNG here, not CPython's Mersenne
 * Twister. Same seed does not give bit-identical shuffle/jitter output
 * between the two. All non-RNG logic (room graph, command handling,
 * trust/suspicion deltas, objective checks, dialogue text) is a direct
 * field-by-field port and is checked for exact match.
 *
 * The PRNG is a hand-written ChaCha20 keystream (RFC 8439), the same
 * algorithm OpenSSL's own EVP_chacha20() wraps, not a call into
 * libcrypto. The host build already links OpenSSL (CMakeLists.txt's
 * CRYPTO_LIB/SSL_LIB), but this guest ELF is cross-compiled by a
 * separate riscv64-musl toolchain with no libcrypto build in it yet
 * (see the plan's step 1 note on the guest toolchain) -- pulling in a
 * cross-compiled OpenSSL for one PRNG is a heavier dependency than the
 * repo's own convention favors (src/gen/xr_grid_entity_packet.c is the
 * same hand-transcribed-algorithm pattern for a different reason).
 * Correctness is checked against RFC 8439's own published test vector
 * in mud/guest/test/chacha20_test.cpp.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "qcbor_value.hpp"

namespace mud {

using mudcbor::Value;

constexpr const char *JSONLD_CONTEXT = "https://v-sekai-multiplayer-fabric.dev/mud/v1";

constexpr const char *OBJECTIVE_GAIN_WATCH_TRUST = "gain_watch_trust";
constexpr const char *OBJECTIVE_IDENTIFY_MARKED_CONTACT = "identify_marked_contact";

/* ---------------------------------------------------------------------
 * ChaCha20 (RFC 8439) block function -- the keystream primitive behind
 * this PRNG. See the file header for why this is hand-written instead
 * of an OpenSSL call.
 * ------------------------------------------------------------------- */
static inline uint32_t rotl32(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void chacha20_quarter_round(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

/* key: 8 words (32 bytes). nonce: 3 words (12 bytes). counter: 1 word.
 * out: 64 bytes of keystream, RFC 8439 section 2.3's exact layout. */
static void chacha20_block(const uint32_t key[8], const uint32_t nonce[3], uint32_t counter, uint8_t out[64]) {
    uint32_t s[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    uint32_t w[16];
    std::memcpy(w, s, sizeof(w));
    for (int round = 0; round < 10; round++) {
        chacha20_quarter_round(w[0], w[4], w[8], w[12]);
        chacha20_quarter_round(w[1], w[5], w[9], w[13]);
        chacha20_quarter_round(w[2], w[6], w[10], w[14]);
        chacha20_quarter_round(w[3], w[7], w[11], w[15]);
        chacha20_quarter_round(w[0], w[5], w[10], w[15]);
        chacha20_quarter_round(w[1], w[6], w[11], w[12]);
        chacha20_quarter_round(w[2], w[7], w[8], w[13]);
        chacha20_quarter_round(w[3], w[4], w[9], w[14]);
    }
    for (int k = 0; k < 16; k++) {
        w[k] += s[k];
    }
    std::memcpy(out, w, 64);
}

/* Deterministic seed -> 32-byte key expansion (splitmix64), not a
 * cryptographic KDF -- this PRNG only needs a unique, well-mixed key
 * per world seed, not resistance to an adversary who knows the seed. */
static void expand_seed_to_key(uint64_t seed, uint32_t key[8]) {
    uint64_t x = seed;
    for (int k = 0; k < 4; k++) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        key[k * 2] = (uint32_t)z;
        key[k * 2 + 1] = (uint32_t)(z >> 32);
    }
}

struct Rng {
    uint32_t key[8];
    uint32_t nonce[3] = {0, 0, 0};
    uint32_t counter = 0;
    uint8_t buf[64];
    size_t buf_pos = 64; /* force a refill on first use */

    explicit Rng(uint64_t seed) { expand_seed_to_key(seed, key); }

    uint8_t next_byte() {
        if (buf_pos >= 64) {
            chacha20_block(key, nonce, counter, buf);
            counter++;
            buf_pos = 0;
        }
        return buf[buf_pos++];
    }

    uint64_t next_u64() {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) {
            v = (v << 8) | next_byte();
        }
        return v;
    }

    /* Inclusive [lo, hi], matches random.Random.randint's contract. */
    int randint(int lo, int hi) {
        if (hi <= lo) {
            return lo;
        }
        uint64_t span = (uint64_t)(hi - lo) + 1;
        return lo + (int)(next_u64() % span);
    }

    template <typename T>
    void shuffle(std::vector<T> &v) {
        for (size_t i = v.size(); i > 1; i--) {
            size_t j = (size_t)(next_u64() % i);
            std::swap(v[i - 1], v[j]);
        }
    }
};

static int clamp(int value, int lo = 0, int hi = 100) {
    return std::max(lo, std::min(hi, value));
}

static std::string normalize_name(std::string token) {
    /* collapse whitespace, trim, lowercase -- matches _normalize_name */
    std::string out;
    bool in_ws = false;
    for (char ch : token) {
        char c = (char)std::tolower((unsigned char)ch);
        if (std::isspace((unsigned char)c)) {
            if (!out.empty()) {
                in_ws = true;
            }
        } else {
            if (in_ws) { out.push_back(' '); in_ws = false; }
            out.push_back(c);
        }
    }
    return out;
}

static std::string normalize_direction(const std::string &token_in) {
    std::string token = normalize_name(token_in);
    static const std::map<std::string, std::string> aliases = {
        {"n", "north"}, {"north", "north"},
        {"s", "south"}, {"south", "south"},
        {"e", "east"}, {"east", "east"},
        {"w", "west"}, {"west", "west"},
        {"d", "down"}, {"down", "down"},
        {"u", "up"}, {"up", "up"},
    };
    auto it = aliases.find(token);
    return it != aliases.end() ? it->second : token;
}

static bool contains_any(const std::string &haystack, const std::vector<std::string> &needles) {
    for (auto &n : needles) {
        if (haystack.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool is_watch_recommendation_attempt(const std::string &message) {
    std::string lower = normalize_name(message);
    static const std::vector<std::string> patterns = {
        "recommend me", "recommend my application", "recommend me to the watch",
        "recommend me for the watch", "sponsor me", "sponsor my application",
        "vouch for me", "endorse me", "put in a word for me",
        "back my application", "support my application",
    };
    return contains_any(lower, patterns);
}

/* ---------------------------------------------------------------------
 * Dialogue classifier -- direct port of classifier.py's
 * _fallback_classification(). Already LLM-free in the original, so
 * this is a straight transcription, not a design change.
 * ------------------------------------------------------------------- */
struct DialogSignal {
    std::string intent = "neutral";
    int sentiment = 0;
    bool direct_objective_probe = false;
    double confidence = 0.55;

    Value to_cbor() const {
        Value v = Value::object();
        v.set("intent", Value::str(intent));
        v.set("sentiment", Value::integer(sentiment));
        v.set("direct_objective_probe", Value::boolean(direct_objective_probe));
        v.set("confidence", Value::integer((int64_t)(confidence * 100))); /* fixed-point x100 */
        return v;
    }
};

static DialogSignal fallback_classification(const std::string &message) {
    std::string lower = normalize_name(message);

    static const std::map<std::string, std::vector<std::string>> keyword_signals = {
        {"praise", {"thank", "grateful", "commend", "honorable", "thank you", "glad"}},
        {"offer_gift", {"give", "coin", "offer", "can i pay", "token", "bribe", "handsome", "gift", "trade"}},
        {"ask_help", {"help", "assist", "please", "could you", "i need", "can you", "escort", "recommend"}},
        {"accusation", {"traitor", "secret", "working with", "supporter", "who is", "which one"}},
        {"threat", {"or else", "danger", "watch yourself", "you better", "i'll report", "not happy"}},
        {"rude", {"stupid", "fool", "liar", "coward", "disgusting"}},
    };

    std::map<std::string, bool> score;
    for (auto &kv : keyword_signals) {
        score[kv.first] = contains_any(lower, kv.second);
    }

    std::string intent;
    if (score["offer_gift"]) {
        intent = "offer_gift";
    }
    else if (score["threat"]) {
        intent = "threat";
    }
    else if (score["accusation"]) intent = "accusation"; /* covers accusation(+ask_help) case too */
    else if (score["ask_help"]) {
        intent = "ask_help";
    }
    else if (score["praise"]) {
        intent = "praise";
    }
    else if (score["rude"]) {
        intent = "rude";
    }
    else {
        intent = (message.find('?') != std::string::npos) ? "ask_info" : "neutral";
    }

    int sentiment = 0;
    if (intent == "praise" || intent == "offer_gift") {
        sentiment = 2;
    }
    else if (intent == "ask_help") {
        sentiment = 1;
    }
    else if (intent == "ask_info" || intent == "neutral") {
        sentiment = 0;
    }
    else if (intent == "deceptive") {
        sentiment = -1;
    }
    else if (intent == "rude" || intent == "accusation") {
        sentiment = -2;
    }
    else if (intent == "threat") {
        sentiment = -3;
    }

    static const std::vector<std::string> probe_terms = {
        "which one", "who is", "marked", "secret", "alignment", "support",
        "broker", "slaver", "identify", "allegiance", "loyal", "sympathy",
        "sympathies", "suspect", "traitor",
    };

    DialogSignal sig;
    sig.intent = intent;
    sig.sentiment = std::max(-3, std::min(3, sentiment));
    sig.direct_objective_probe = contains_any(lower, probe_terms);
    sig.confidence = 0.55;
    return sig;
}

/* ---------------------------------------------------------------------
 * World data -- direct transcription of state_machine.py's
 * _ROOM_TEMPLATES / _ITEM_DESCRIPTIONS / _NPC_TEMPLATES /
 * _NPC_ALIASES / _COMMAND_ALIASES.
 * ------------------------------------------------------------------- */
struct Room {
    std::string name;
    std::string desc;
    std::map<std::string, std::string> exits; /* direction -> room key */
    std::vector<std::string> npcs;
    std::vector<std::string> items;
};

struct NpcTemplate {
    std::string key, name, role, demeanor;
    int base_trust, base_suspicion;
    std::string base_dialogue, loyalty_note, investigation_hint;
};

struct NpcState {
    const NpcTemplate *tmpl;
    int trust, suspicion;
    bool alignment_marked = false;
    std::vector<std::string> clues;
    int talk_count = 0;

    std::pair<int, int> apply_signal(const DialogSignal &signal) {
        int trust_delta = signal.sentiment;
        int suspicion_delta = 0;
        if (signal.direct_objective_probe) {
            suspicion_delta += 2;
        }
        if (signal.intent == "offer_gift") {
            trust_delta += 2;
        } else if (signal.intent == "threat" || signal.intent == "rude" || signal.intent == "accusation") {
            trust_delta -= 1;
            suspicion_delta += 2;
        } else if (signal.intent == "deceptive") {
            trust_delta -= 1;
            suspicion_delta += 1;
        }
        int before_trust = trust, before_susp = suspicion;
        trust = clamp(trust + trust_delta);
        suspicion = clamp(suspicion + suspicion_delta);
        talk_count++;
        return {trust - before_trust, suspicion - before_susp};
    }
};

static const std::vector<std::pair<std::string, Room>> &room_templates() {
    static const std::vector<std::pair<std::string, Room>> rooms = {
        {"city_gate", {"Middleham City Gate",
            "A heavy city gate marks the border. Patrol banners flutter.",
            {{"north", "main_square"}}, {}, {"guard_token", "old_map"}}},
        {"main_square", {"Middleham Main Square",
            "A civic square with notices and loud vendor calls.",
            {{"south", "city_gate"}, {"north", "guard_barracks"}, {"east", "market_street"}, {"west", "tavern"}},
            {}, {"street_crystal"}}},
        {"guard_barracks", {"Guard Barracks Court",
            "Barracks and a command circle, with law posted everywhere.",
            {{"south", "main_square"}, {"east", "residential_street"}, {"north", "guild_court"}},
            {"captain"}, {"signet_ring"}}},
        {"guild_court", {"Guild Court",
            "A narrow court where tariffs and petitions stack high.",
            {{"south", "guard_barracks"}, {"east", "outskirt_road"}}, {}, {"guild_coin"}}},
        {"market_street", {"Merchant Quarter",
            "Crowded stalls with grain, cloth, and tools.",
            {{"west", "main_square"}, {"north", "merchant_hall"}, {"east", "temple_entry"}},
            {"merchant"}, {"tariff_letter"}}},
        {"merchant_hall", {"Merchant Hall",
            "A cramped counting room with old ledgers.",
            {{"south", "market_street"}, {"east", "outskirt_road"}}, {}, {"sealed_letter"}}},
        {"tavern", {"Tarnished Lantern Tavern",
            "Smoke, low voices, and quick rumors.",
            {{"east", "main_square"}, {"north", "temple_entry"}}, {"keeper"}, {"rumor_scroll"}}},
        {"temple_entry", {"Temple Steps",
            "Stone steps and quiet argument beneath a lit shrine.",
            {{"west", "market_street"}, {"south", "tavern"}, {"north", "temple_inner"}},
            {}, {"prayer_beads", "temple_pass"}}},
        {"temple_inner", {"Temple Inner Court",
            "A quiet brazierside court with marked circles in ash.",
            {{"south", "temple_entry"}}, {"peasant"}, {"altar_chalk"}}},
        {"residential_street", {"Residential Street",
            "Narrow homes with shutters and narrow sight lines.",
            {{"west", "guard_barracks"}, {"north", "temple_entry"}, {"east", "outskirt_road"}},
            {}, {"cloth_scarf"}}},
        {"outskirt_road", {"Road to the Outskirts",
            "A shallow ditch and dark tree line.",
            {{"west", "residential_street"}, {"south", "forest_rim"}, {"north", "merchant_hall"}, {"east", "forest_rim"}},
            {}, {"rusted_blade"}}},
        {"forest_rim", {"Forest Rim",
            "Tall pines and a press of silence.",
            {{"north", "outskirt_road"}, {"south", "city_gate"}}, {}, {"charcoal_stone"}}},
    };
    return rooms;
}

static const std::map<std::string, std::string> &item_descriptions() {
    static const std::map<std::string, std::string> items = {
        {"guard_token", "A stamped pass token used by patrol officers."},
        {"old_map", "A weathered map of nearby roads."},
        {"street_crystal", "A decorative stone embedded in the square."},
        {"signet_ring", "A command seal used by guards."},
        {"guild_coin", "A small minted token for market officials."},
        {"tariff_letter", "A bulletin about tariff pressure and unrest."},
        {"sealed_letter", "A wax-sealed document with route notes."},
        {"rumor_scroll", "Encoded rumor notes from the city."},
        {"prayer_beads", "Wooden beads for temple prayers."},
        {"temple_pass", "Temporary access to temple sections."},
        {"altar_chalk", "Gray chalk used to mark witness circles."},
        {"cloth_scarf", "A smoke-smelling scarf with symbols."},
        {"rusted_blade", "An old but serviceable blade."},
        {"charcoal_stone", "A stone that flakes into soot."},
    };
    return items;
}

static const std::map<std::string, NpcTemplate> &npc_templates() {
    static const std::map<std::string, NpcTemplate> npcs = {
        {"captain", {"captain", "Captain Ser Alarik", "Watch Officer", "disciplined, formal", 58, 22,
            "The captain nods and speaks in clipped commands.",
            "He rewards lawful conduct and clear behavior.",
            "Captain Ser Alarik says: 'I can recommend someone who behaves with restraint.'"}},
        {"keeper", {"keeper", "Hale the Keeper", "Tavern Keeper", "friendly but cautious", 50, 30,
            "The tavern keeper wipes a cup and studies you closely.",
            "He remembers favors and forgets small insults.",
            "Hale says: 'There are loyal lawkeepers and others who move like shadows.'"}},
        {"merchant", {"merchant", "Bran the Merchant", "Road Merchant", "practical and money-minded", 52, 28,
            "Bran checks his ledger and responds with measured caution.",
            "He values stable routes and low risk.",
            "Bran says: 'A broker brings silver from strangers and asks wrong questions.'"}},
        {"peasant", {"peasant", "Yelena the Peasant Freedman", "Former Bonded Laborer", "grateful, guarded, alert", 46, 34,
            "Yelena glances over her shoulder before answering.",
            "She watches the outskirt road and fears slaver return.",
            "Yelena whispers: 'Someone tracks who comes and goes at dusk near the broker.'"}},
    };
    return npcs;
}

static const std::map<std::string, std::set<std::string>> &npc_aliases() {
    static const std::map<std::string, std::set<std::string>> aliases = {
        {"captain", {"captain", "alarik", "officer", "guard"}},
        {"keeper", {"keeper", "tavern", "barkeep"}},
        {"merchant", {"merchant", "bran", "trader"}},
        {"peasant", {"peasant", "yelena", "freedman"}},
    };
    return aliases;
}

static const std::map<std::string, std::set<std::string>> &command_aliases() {
    static const std::map<std::string, std::set<std::string>> aliases = {
        {"look", {"look", "l"}},
        {"go", {"go", "move"}},
        {"talk", {"talk", "say", "ask"}},
        {"examine", {"examine", "inspect", "x"}},
        {"take", {"take", "get", "pick"}},
        {"give", {"give", "offer"}},
        {"use", {"use", "apply"}},
    };
    return aliases;
}

static std::string normalize_command(const std::string &raw) {
    std::string command = normalize_name(raw);
    for (auto &kv : command_aliases()) {
        if (kv.second.count(command)) {
            return kv.first;
        }
    }
    return command;
}

/* ---------------------------------------------------------------------
 * PlayerAction / MiddlehamStateMachine -- direct port of
 * state_machine.py's PlayerAction and MiddlehamStateMachine.
 * ------------------------------------------------------------------- */
struct PlayerAction {
    std::string command;
    std::vector<std::string> args;
    std::string message;
};

class MiddlehamStateMachine {
public:
    MiddlehamStateMachine(uint64_t seed, std::string objective, std::string marked_target_hint, int max_turns)
        : seed_(seed), rng_(seed), objective_(std::move(objective)), max_turns_(max_turns),
          current_room_("city_gate") {
        objective_state_watch_talks_ = 0;
        objective_state_watch_recommendation_requests_ = 0;
        objective_state_direct_objective_probes_ = 0;
        objective_state_clue_count_ = 0;
        objective_state_invalid_actions_ = 0;
        objective_state_inventory_touched_ = 0;
        location_visits_.push_back(current_room_);

        clone_rooms();
        seed_npcs();
        seed_marked(marked_target_hint);
        for (auto &kv : npc_templates()) {
            suspect_scores_[kv.first] = 0;
        }
    }

    bool objective_complete() const {
        if (objective_ == OBJECTIVE_GAIN_WATCH_TRUST) {
            auto it = npcs_.find("captain");
            return it != npcs_.end() && it->second.trust >= 75 &&
                   objective_state_watch_talks_ >= 2 &&
                   objective_state_watch_recommendation_requests_ >= 1;
        }
        if (objective_ == OBJECTIVE_IDENTIFY_MARKED_CONTACT) {
            return objective_state_direct_objective_probes_ >= 4 &&
                   has_unique_top_suspect() &&
                   infer_suspect() == marked_target_ &&
                   objective_state_clue_count_ >= 1;
        }
        return false;
    }

    bool is_complete() const { return turn_ >= max_turns_ || finished_; }

    Value step(const PlayerAction &action) {
        turn_++;
        std::string pre_room = current_room_;
        Room &room = rooms_.at(current_room_);
        std::vector<std::string> lines;
        Value state_delta = Value::object();
        std::string command = action.command;
        bool valid = true;
        Value npc_reactions = Value::array();
        Value dialogue_signal = Value::null();

        if (command == "look") {
            lines.push_back(room_description(true));
        } else if (command == "go") {
            if (action.args.empty()) {
                valid = false;
                lines.push_back("Go where?");
                objective_state_invalid_actions_++;
            } else {
                std::string direction = normalize_direction(action.args[0]);
                auto exit_it = room.exits.find(direction);
                if (exit_it == room.exits.end()) {
                    valid = false;
                    lines.push_back("You cannot go that way.");
                    objective_state_invalid_actions_++;
                } else {
                    current_room_ = exit_it->second;
                    location_visits_.push_back(current_room_);
                    Value move = Value::object();
                    move.set("from", Value::str(pre_room));
                    move.set("to", Value::str(current_room_));
                    move.set("direction", Value::str(direction));
                    state_delta.set("move", move);
                    lines.push_back("You go " + direction + " to " + rooms_.at(current_room_).name + ".");
                    lines.push_back(room_description(true));
                }
            }
        } else if (command == "talk") {
            if (action.args.empty()) {
                valid = false;
                lines.push_back("Talk to whom?");
                objective_state_invalid_actions_++;
            } else {
                std::string target = resolve_npc(action.args[0]);
                if (target.empty()) {
                    valid = false;
                    lines.push_back("No one by that name is here.");
                    objective_state_invalid_actions_++;
                } else {
                    std::string message = action.message;
                    if (message.empty() && action.args.size() > 1) {
                        for (size_t k = 1; k < action.args.size(); k++) {
                            if (k > 1) {
                                message += " ";
                            }
                            message += action.args[k];
                        }
                    }
                    DialogSignal signal = fallback_classification(message);
                    std::string speech = talk(target, message, signal);
                    auto delta = npcs_.at(target).apply_signal(signal);
                    Value reaction = Value::object();
                    reaction.set("npc", Value::str(target));
                    reaction.set("speech", Value::str(speech));
                    Value sd = Value::object();
                    sd.set("trust_delta", Value::integer(delta.first));
                    sd.set("suspicion_delta", Value::integer(delta.second));
                    reaction.set("state_delta", sd);
                    reaction.set("signal", signal.to_cbor());
                    npc_reactions.arr.push_back(reaction);
                    dialogue_signal = signal.to_cbor();
                    lines.push_back(speech);
                    state_delta.set("npc_" + target, sd);

                    if (target == "captain") {
                        objective_state_watch_talks_++;
                        if (is_watch_recommendation_attempt(message)) {
                            objective_state_watch_recommendation_requests_++;
                        }
                    }
                    if (signal.direct_objective_probe) {
                        objective_state_direct_objective_probes_++;
                        suspect_scores_[target] += 2;
                        if (target == marked_target_) {
                            suspect_scores_[target] += 3;
                        }
                    }
                    if (std::find(talked_npcs_.begin(), talked_npcs_.end(), target) == talked_npcs_.end()) {
                        talked_npcs_.push_back(target);
                    }
                }
            }
        } else if (command == "examine") {
            if (action.args.empty()) {
                valid = false;
                lines.push_back("Examine what?");
                objective_state_invalid_actions_++;
            } else {
                lines.push_back(examine(action.args[0]));
            }
        } else if (command == "take") {
            if (action.args.empty()) {
                valid = false;
                lines.push_back("Take what?");
                objective_state_invalid_actions_++;
            } else {
                std::string item = normalize_item(action.args[0]);
                auto &room_items = rooms_.at(current_room_).items;
                auto it = std::find(room_items.begin(), room_items.end(), item);
                if (it != room_items.end()) {
                    room_items.erase(it);
                    if (std::find(inventory_.begin(), inventory_.end(), item) == inventory_.end()) {
                        inventory_.push_back(item);
                        objective_state_inventory_touched_++;
                    }
                    lines.push_back("You take " + item + ".");
                } else {
                    valid = false;
                    lines.push_back("That item is not here.");
                    objective_state_invalid_actions_++;
                }
            }
        } else if (command == "give") {
            if (action.args.size() < 2) {
                valid = false;
                lines.push_back("Give what to whom?");
                objective_state_invalid_actions_++;
            } else {
                std::string item = normalize_item(action.args[0]);
                std::string target = resolve_npc(action.args.back());
                if (target.empty()) {
                    valid = false;
                    lines.push_back("No one here can receive it.");
                    objective_state_invalid_actions_++;
                } else if (std::find(inventory_.begin(), inventory_.end(), item) == inventory_.end()) {
                    valid = false;
                    lines.push_back("You do not have that item.");
                    objective_state_invalid_actions_++;
                } else {
                    inventory_.erase(std::find(inventory_.begin(), inventory_.end(), item));
                    objective_state_inventory_touched_++;
                    NpcState &npc = npcs_.at(target);
                    npc.trust = clamp(npc.trust + 3);
                    npc.suspicion = clamp(npc.suspicion - 1);
                    suspect_scores_[target] += 1;
                    if (target == "captain") {
                        lines.push_back("You give " + item + " to Captain Ser Alarik. He softens slightly.");
                    }
                    else if (target == "keeper") {
                        lines.push_back("You give " + item + " to Hale. He nods and relaxes.");
                    }
                    else if (target == "merchant") {
                        lines.push_back("You give " + item + " to Bran. He shares a safer route.");
                    }
                    else {
                        lines.push_back("You give " + item + " to Yelena and she seems calmer.");
                    }
                }
            }
        } else if (command == "use") {
            if (action.args.empty()) {
                valid = false;
                lines.push_back("Use what?");
                objective_state_invalid_actions_++;
            } else {
                std::string item = normalize_item(action.args[0]);
                if (std::find(inventory_.begin(), inventory_.end(), item) == inventory_.end()) {
                    valid = false;
                    lines.push_back("You do not have that item.");
                    objective_state_invalid_actions_++;
                } else if (item == "guard_token" && current_room_ == "city_gate") {
                    lines.push_back("The guard token lets you pass more freely through check points.");
                } else if (item == "temple_pass" && (current_room_ == "temple_entry" || current_room_ == "temple_inner")) {
                    lines.push_back("The temple pass opens access to inner sections.");
                } else if (item == "sealed_letter") {
                    lines.push_back("You open the sealed letter and pull route notes out of it.");
                    objective_state_clue_count_++;
                } else if (item == "rusted_blade") {
                    lines.push_back("You grip the blade. Nearby NPCs keep their distance.");
                } else {
                    lines.push_back("You use " + item + ", but nothing obvious changes.");
                }
            }
        } else {
            valid = false;
            lines.push_back("Unknown command.");
            objective_state_invalid_actions_++;
        }

        if (objective_complete()) {
            finished_ = true;
            lines.push_back(objective_ == OBJECTIVE_GAIN_WATCH_TRUST
                ? "Captain Ser Alarik indicates he can sponsor your application."
                : "You have enough evidence to identify a likely Marked contact.");
        }

        std::string narration;
        for (size_t k = 0; k < lines.size(); k++) {
            if (k) {
                narration += " ";
            }
            narration += lines[k];
        }

        Value result = Value::object();
        result.set("@context", Value::str(JSONLD_CONTEXT));
        result.set("@type", Value::str("MudTurnResult"));
        result.set("turn", Value::integer(turn_));
        Value parsed_action = Value::object();
        parsed_action.set("command", Value::str(command));
        Value args_v = Value::array();
        for (auto &a : action.args) {
            args_v.arr.push_back(Value::str(a));
        }
        parsed_action.set("args", args_v);
        parsed_action.set("message", Value::str(action.message));
        result.set("parsed_action", parsed_action);
        result.set("pre_room", Value::str(pre_room));
        result.set("post_room", Value::str(current_room_));
        result.set("narration", Value::str(narration));
        result.set("valid", Value::boolean(valid));
        result.set("dialogue_signal", dialogue_signal);
        result.set("npc_reactions", npc_reactions);
        result.set("state_delta", state_delta);
        result.set("objective_complete", Value::boolean(objective_complete()));
        result.set("finished", Value::boolean(is_complete()));
        return result;
    }

private:
    void clone_rooms() {
        for (auto &kv : room_templates()) {
            rooms_[kv.first] = kv.second;
        }

        std::vector<std::string> move_items = {"guard_token", "tariff_letter", "old_map"};
        rng_.shuffle(move_items);
        std::vector<std::string> keys;
        for (auto &kv : room_templates()) {
            keys.push_back(kv.first);
        }
        for (size_t idx = 0; idx < move_items.size(); idx++) {
            const std::string &item = move_items[idx];
            for (auto &kv : rooms_) {
                auto &items = kv.second.items;
                auto it = std::find(items.begin(), items.end(), item);
                if (it != items.end()) {
                    items.erase(it);
                }
            }
            const std::string &target = keys[(idx + (size_t)seed_) % keys.size()];
            auto &target_items = rooms_.at(target).items;
            if (std::find(target_items.begin(), target_items.end(), item) == target_items.end()) {
                target_items.push_back(item);
            }
        }
    }

    void seed_npcs() {
        for (auto &kv : npc_templates()) {
            int jitter = rng_.randint(-3, 3);
            NpcState st;
            st.tmpl = &kv.second;
            st.trust = clamp(kv.second.base_trust + jitter);
            st.suspicion = clamp(kv.second.base_suspicion + rng_.randint(-2, 2));
            npcs_[kv.first] = st;
        }
    }

    void seed_marked(const std::string &hint) {
        static const std::vector<std::string> candidates = {"keeper", "merchant", "peasant"};
        std::string target = hint;
        if (std::find(candidates.begin(), candidates.end(), target) == candidates.end()) {
            target = candidates[seed_ % candidates.size()];
        }
        marked_target_ = target;
        npcs_.at(target).alignment_marked = true;
    }

    bool has_unique_top_suspect() const {
        if (suspect_scores_.empty()) {
            return false;
        }
        int top = -1;
        int count_at_top = 0;
        for (auto &kv : suspect_scores_) {
            top = std::max(top, kv.second);
        }
        if (top == 0) {
            return false;
        }
        for (auto &kv : suspect_scores_) {
            if (kv.second == top) {
                count_at_top++;
            }
        }
        return count_at_top == 1;
    }

    std::string infer_suspect() const {
        std::string best;
        int best_score = -1;
        for (auto &kv : suspect_scores_) {
            if (kv.second > best_score) { best_score = kv.second; best = kv.first; }
        }
        return best;
    }

    std::string room_description(bool verbose) const {
        const Room &room = rooms_.at(current_room_);
        std::string exits_joined;
        {
            std::vector<std::string> sorted_exits;
            for (auto &kv : room.exits) {
                sorted_exits.push_back(kv.first);
            }
            std::sort(sorted_exits.begin(), sorted_exits.end());
            for (size_t k = 0; k < sorted_exits.size(); k++) {
                if (k) {
                    exits_joined += ", ";
                }
                exits_joined += sorted_exits[k];
            }
        }
        std::string base = room.name + ": " + room.desc + ". Exits: " + exits_joined + ".";
        if (!verbose) {
            return base;
        }
        if (!room.npcs.empty()) {
            std::string names;
            for (size_t k = 0; k < room.npcs.size(); k++) {
                if (k) {
                    names += ", ";
                }
                names += npcs_.at(room.npcs[k]).tmpl->name;
            }
            base += " NPCs: " + names + ".";
        }
        if (!room.items.empty()) {
            std::string items_joined;
            for (size_t k = 0; k < room.items.size(); k++) {
                if (k) {
                    items_joined += ", ";
                }
                items_joined += room.items[k];
            }
            base += " Items: " + items_joined + ".";
        }
        if (!inventory_.empty()) {
            std::string inv_joined;
            for (size_t k = 0; k < inventory_.size(); k++) {
                if (k) {
                    inv_joined += ", ";
                }
                inv_joined += inventory_[k];
            }
            base += " Inventory: " + inv_joined + ".";
        }
        return base;
    }

    std::string examine(const std::string &raw) const {
        std::string token = normalize_item(raw);
        const Room &room = rooms_.at(current_room_);
        std::string npc = resolve_npc(token);
        if (!npc.empty() && std::find(room.npcs.begin(), room.npcs.end(), npc) != room.npcs.end()) {
            const NpcTemplate &t = *npcs_.at(npc).tmpl;
            return t.name + ": " + t.role + ", " + t.demeanor + ". " + t.loyalty_note;
        }
        bool in_room = std::find(room.items.begin(), room.items.end(), token) != room.items.end();
        bool in_inv = std::find(inventory_.begin(), inventory_.end(), token) != inventory_.end();
        if (in_room || in_inv) {
            auto it = item_descriptions().find(token);
            return it != item_descriptions().end() ? it->second : "The item is ordinary.";
        }
        return "There is nothing worth examining there.";
    }

    static std::string normalize_item(const std::string &raw) {
        std::string token = normalize_name(raw);
        for (auto &c : token) {
            if (c == ' ') {
                c = '_';
            }
        }
        static const std::map<std::string, std::string> aliases = {
            {"guardtoken", "guard_token"}, {"oldmap", "old_map"},
            {"tariffletter", "tariff_letter"}, {"sealedletter", "sealed_letter"},
            {"rumorscroll", "rumor_scroll"}, {"prayerbeads", "prayer_beads"},
            {"templepass", "temple_pass"}, {"altarchalk", "altar_chalk"},
            {"clothscarf", "cloth_scarf"}, {"rustedblade", "rusted_blade"},
            {"charcoalstone", "charcoal_stone"},
        };
        auto it = aliases.find(token);
        return it != aliases.end() ? it->second : token;
    }

    std::string resolve_npc(const std::string &raw) const {
        std::string token = normalize_name(raw);
        const auto &present = rooms_.at(current_room_).npcs;
        if (std::find(present.begin(), present.end(), token) != present.end()) {
            return token;
        }
        for (auto &key : present) {
            auto ait = npc_aliases().find(key);
            if (ait != npc_aliases().end() && ait->second.count(token)) {
                return key;
            }
            std::string lname = npcs_.at(key).tmpl->name;
            for (auto &c : lname) {
                c = (char)std::tolower((unsigned char)c);
            }
            if (lname.find(token) != std::string::npos) {
                return key;
            }
        }
        for (auto &kv : npc_aliases()) {
            if (kv.second.count(token) &&
                std::find(present.begin(), present.end(), kv.first) != present.end()) {
                return kv.first;
            }
        }
        return "";
    }

    std::string talk(const std::string &npc_key, const std::string &message, const DialogSignal &signal) {
        NpcState &npc = npcs_.at(npc_key);
        std::string msg_lower = normalize_name(message);
        std::vector<std::string> lines;

        if (npc.suspicion >= 70) {
            lines.push_back(npc.tmpl->name + " keeps their eyes down and answers in clipped syllables.");
        } else if (npc.suspicion >= 50) {
            lines.push_back(npc.tmpl->name + " looks uneasy. " + npc.tmpl->base_dialogue);
        } else if (npc.trust >= 70) {
            lines.push_back(npc.tmpl->name + " greets you with visible ease. " + npc.tmpl->base_dialogue);
        } else if (npc.trust >= 55) {
            lines.push_back(npc.tmpl->base_dialogue);
        } else {
            lines.push_back(npc.tmpl->name + " is guarded but civil. " + npc.tmpl->base_dialogue);
        }

        if (signal.intent == "praise") {
            lines.push_back(npc.tmpl->name + " seems pleased by the kind words and their manner warms slightly.");
        } else if (signal.intent == "offer_gift") {
            lines.push_back(npc.tmpl->name + " accepts the gesture. Their posture opens a little.");
        } else if (signal.intent == "ask_help") {
            lines.push_back(npc.trust >= 50 ? "They seem willing to assist, if cautiously."
                                             : "They hesitate, not yet sure whether to trust you.");
        } else if (signal.intent == "threat") {
            lines.push_back(npc.tmpl->name + " stiffens. 'Threats won't get you far in this city. I'd watch that tone.'");
        } else if (signal.intent == "rude") {
            lines.push_back(npc.tmpl->name + " takes a step back, visibly offended.");
        } else if (signal.intent == "accusation") {
            lines.push_back(npc.suspicion >= 40
                ? npc.tmpl->name + " bristles. 'Be very careful what you imply, stranger.'"
                : npc.tmpl->name + " frowns deeply at the insinuation and says nothing.");
        }

        if (signal.direct_objective_probe) {
            if (npc.alignment_marked && npc.trust >= 35) {
                if (std::find(npc.clues.begin(), npc.clues.end(), npc.tmpl->investigation_hint) == npc.clues.end()) {
                    npc.clues.push_back(npc.tmpl->investigation_hint);
                    knowledge_.push_back(npc.tmpl->investigation_hint);
                    objective_state_clue_count_++;
                }
                lines.push_back(npc.tmpl->investigation_hint);
            } else if (npc.suspicion >= 50) {
                lines.push_back("They clam up entirely, eyes narrowing. That subject is clearly closed.");
            } else {
                lines.push_back("They shift uncomfortably and change the subject without answering.");
            }
        } else if (npc_key == "captain") {
            if (contains_any(msg_lower, {"recommend", "join", "enlist", "apply", "watchman"})) {
                if (npc.trust >= 65) {
                    lines.push_back("The captain nods slowly. 'You have conducted yourself with restraint. "
                        "Come back after you have proven that is not circumstance.'");
                } else if (npc.trust >= 50) {
                    lines.push_back("'I need to see more from you before I put my name behind anyone. "
                        "Keep your nose clean and your dealings straight.'");
                } else {
                    lines.push_back("'I don't recommend people I barely know. Earn trust first.'");
                }
            } else if (contains_any(msg_lower, {"law", "order", "rule", "compact"})) {
                lines.push_back("'The law is the only thing standing between order and ruin in this city. "
                    "Every exception to it is a crack in the wall.'");
            } else if (contains_any(msg_lower, {"tariff", "merchant", "guild", "trade"})) {
                lines.push_back("'Trade tensions are not my jurisdiction. "
                    "My concern is law-breaking, not commerce. Don't confuse the two.'");
            } else if (contains_any(msg_lower, {"marked", "shadow", "secret"})) {
                lines.push_back("'I enforce what is written. Anything beyond the letter of the law is not my problem -- "
                    "unless someone makes it one.'");
            }
        } else if (npc_key == "keeper") {
            if (contains_any(msg_lower, {"tariff", "price", "tax", "cost"})) {
                lines.push_back("Hale sighs and sets down his cup. 'Chancellor's tariffs are bleeding the quarter dry. "
                    "Half my regulars can't afford a warm meal anymore. Someone's getting rich off this.'");
            } else if (contains_any(msg_lower, {"marked", "secret", "shadow", "hidden", "contact"})) {
                lines.push_back(npc.trust >= 55
                    ? "Hale leans close and drops his voice. "
                      "'There are people who move quietly through this city. I don't ask their names. "
                      "A man who does would find the right door eventually.'"
                    : "Hale wipes the counter slowly and doesn't meet your eyes. "
                      "'Can't help you with that. Ask someone else.'");
            } else if (contains_any(msg_lower, {"watchman", "captain", "alarik", "law"})) {
                lines.push_back("'Ser Alarik? Honest man. Hard man. Follows the law to the letter -- "
                    "which is a comfort to some and a problem for others.'");
            } else if (contains_any(msg_lower, {"rumor", "news", "heard", "happening"})) {
                lines.push_back("'Word is the outskirt road's been watched lately. "
                    "Travelers reporting unfamiliar faces near the tree line at dusk. "
                    "Nobody official. Nobody local.'");
            }
        } else if (npc_key == "merchant") {
            if (contains_any(msg_lower, {"escort", "route", "road", "path", "travel"})) {
                lines.push_back(npc.trust >= 50
                    ? "Bran lowers his voice. 'Southern path along the outskirt road is quietest right now. "
                      "Go at midday, not dusk. Something moves in those trees after dark -- "
                      "and it is not wildlife.'"
                    : "'I don't hand out route advice to strangers. Too much risk in that.'");
            } else if (contains_any(msg_lower, {"tariff", "trade", "price", "tax", "goods"})) {
                lines.push_back("Bran's jaw tightens. 'The chancellor's tariffs are making honest trade impossible. "
                    "Meanwhile the black-market brokers operate without interference. Someone is looking away on purpose.'");
            } else if (contains_any(msg_lower, {"broker", "silver", "cargo", "deal", "shipment"})) {
                lines.push_back("Bran's eyes narrow. 'There's a broker working the outskirt market. "
                    "Pays in silver, no questions asked -- but asks a great many wrong questions about "
                    "shipment contents and schedules. I keep my distance from that one.'");
            }
        } else if (npc_key == "peasant") {
            if (contains_any(msg_lower, {"slaver", "broker", "shuddeni", "bonded", "slave"})) {
                lines.push_back(npc.trust >= 50
                    ? "Yelena glances toward the door before answering, voice barely above a whisper. "
                      "'Someone comes near the outskirt road at dusk. Watches who enters and leaves. "
                      "Not a merchant. Not a watchman. Asks about people's origins.'"
                    : "Yelena goes very still. "
                      "'I don't know anything about that. Please -- do not ask me that again.'");
            } else if (contains_any(msg_lower, {"free", "freedom", "safe", "help", "protect"})) {
                lines.push_back("'Freedom is fragile here. "
                    "The law protects some of us and quietly ignores others -- "
                    "depending entirely on where you came from and who brought you.'");
            } else if (contains_any(msg_lower, {"watchman", "captain", "law", "rule"})) {
                lines.push_back("'The Watch enforces what is written. "
                    "What is written is not always just. Those are two different things.'");
            } else if (contains_any(msg_lower, {"outskirt", "road", "forest", "dusk", "night"})) {
                lines.push_back("Yelena's eyes flick toward the exit. "
                    "'Stay away from the outskirt road after sundown. "
                    "Whatever business happens there -- it is not business you want to witness.'");
            }
        }

        std::string out;
        for (size_t k = 0; k < lines.size(); k++) {
            if (k) {
                out += " ";
            }
            out += lines[k];
        }
        return out;
    }

    uint64_t seed_;
    Rng rng_;
    std::string objective_;
    int max_turns_;
    int turn_ = 0;
    std::string current_room_;
    std::vector<std::string> inventory_;
    std::vector<std::string> knowledge_;
    std::vector<std::string> location_visits_;
    std::vector<std::string> talked_npcs_;
    std::map<std::string, Room> rooms_;
    std::map<std::string, NpcState> npcs_;
    std::map<std::string, int> suspect_scores_;
    std::string marked_target_;
    bool finished_ = false;

    int objective_state_watch_talks_;
    int objective_state_watch_recommendation_requests_;
    int objective_state_direct_objective_probes_;
    int objective_state_clue_count_;
    int objective_state_invalid_actions_;
    int objective_state_inventory_touched_;
};

/* Single active session per guest instance -- one orchestrator process
 * per MUD session, matching the Python original's single-player-per-
 * run model (plan step 2). */
static MiddlehamStateMachine *g_session = nullptr;

static PlayerAction parse_command(const Value &cmd) {
    PlayerAction action;
    action.command = normalize_command(cmd.get_str("command", "look"));
    const Value *args_v = cmd.get("args");
    if (args_v && args_v->type == mudcbor::Type::Array) {
        for (auto &a : args_v->arr) {
            if (a.type == mudcbor::Type::Str) {
                action.args.push_back(normalize_name(a.s));
            }
        }
    }
    action.message = cmd.get_str("message", "");
    return action;
}

} // namespace mud

/* ---------------------------------------------------------------------
 * vmcall()-reachable entry points.
 *
 * Output goes into g_mud_out_buffer, a fixed-address global the host
 * orchestrator resolves once at load time via
 * Machine<W>::memory.resolve_address("g_mud_out_buffer") (the same
 * symbol-resolution machinery vmcall(const char *funcname, ...) itself
 * uses for function names). This avoids needing the guest to hand the
 * host a pointer through a vmcall integer-return channel: only one
 * address needs resolving per session, not once per turn. Input bytes
 * still cross via vmcall()'s own std::vector<uint8_t> argument
 * marshaling (Machine<W>::setup_call auto stack_push()es it and passes
 * the resulting guest address, per lib/libriscv/machine_vmcall.hpp) --
 * so only the length needs to be a second explicit argument.
 * ------------------------------------------------------------------- */
constexpr size_t MUD_OUT_BUFFER_CAP = 65536;
extern "C" uint8_t g_mud_out_buffer[MUD_OUT_BUFFER_CAP];
uint8_t g_mud_out_buffer[MUD_OUT_BUFFER_CAP];

extern "C" {

long mud_boot(const uint8_t *cfg_cbor, size_t cfg_len) {
    using namespace mud;
    mudcbor::Value cfg = mudcbor::decode(cfg_cbor, cfg_len);
    uint64_t seed = (uint64_t)cfg.get_int("seed", 0);
    std::string objective = cfg.get_str("objective", OBJECTIVE_GAIN_WATCH_TRUST);
    std::string marked_hint = cfg.get_str("marked_target", "");
    int max_turns = (int)cfg.get_int("max_turns", 50);

    delete g_session;
    g_session = new MiddlehamStateMachine(seed, objective, marked_hint, max_turns);

    Value ack = Value::object();
    ack.set("@context", Value::str(JSONLD_CONTEXT));
    ack.set("@type", Value::str("MudBootAck"));
    ack.set("ok", Value::boolean(true));
    std::vector<uint8_t> encoded = mudcbor::encode(ack);
    if (encoded.size() > MUD_OUT_BUFFER_CAP) {
        return -1;
    }
    std::memcpy(g_mud_out_buffer, encoded.data(), encoded.size());
    return (long)encoded.size();
}

long mud_step(const uint8_t *cmd_cbor, size_t cmd_len) {
    using namespace mud;
    if (g_session == nullptr) {
        return -1;
    }
    mudcbor::Value cmd = mudcbor::decode(cmd_cbor, cmd_len);
    PlayerAction action = parse_command(cmd);
    Value result = g_session->step(action);
    std::vector<uint8_t> encoded = mudcbor::encode(result);
    if (encoded.size() > MUD_OUT_BUFFER_CAP) {
        return -1;
    }
    std::memcpy(g_mud_out_buffer, encoded.data(), encoded.size());
    return (long)encoded.size();
}

} // extern "C"

/* This guest ELF is never run start-to-finish -- the host orchestrator
 * vmcall()s mud_boot()/mud_step() directly by symbol name (see
 * mud/orchestrator/). main() only exists so this links as a static
 * executable at all; libriscv/rvlinux still runs libc/crt startup
 * (global constructors, if any -- this file has none, only function-
 * local statics with lazy init) before the first vmcall(), then the
 * guest sits idle at main()'s return until the next vmcall().
 *
 * MUD_GUEST_NO_MAIN lets a native test harness (mud/guest/test/) link
 * this file directly and supply its own main() instead. */
#ifndef MUD_GUEST_NO_MAIN
int main() { return 0; }
#endif
