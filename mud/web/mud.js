// Middleham MUD website UI. No accounts, no login (per the project's
// own "no need for oauth" decision) -- a random session id is minted
// once in this browser and reused, matching what src/mud/mud_session.c
// expects as a session_id string.

(function () {
  "use strict";

  const SESSION_KEY = "mud_session_id";
  function getOrCreateSessionId() {
    let id = localStorage.getItem(SESSION_KEY);
    if (!id) {
      id = "web-" + Math.random().toString(36).slice(2) + Date.now().toString(36);
      localStorage.setItem(SESSION_KEY, id);
    }
    return id;
  }
  const sessionId = getOrCreateSessionId();

  const logEl = document.getElementById("log");
  const form = document.getElementById("commandForm");
  const input = document.getElementById("commandInput");

  function appendTurn(text, meta, invalid) {
    const div = document.createElement("div");
    div.className = "turn";
    const body = document.createElement("div");
    body.textContent = text;
    if (invalid) body.className = "invalid";
    div.appendChild(body);
    if (meta) {
      const metaEl = document.createElement("div");
      metaEl.className = "meta";
      metaEl.textContent = meta;
      div.appendChild(metaEl);
    }
    logEl.appendChild(div);
    logEl.scrollTop = logEl.scrollHeight;
  }

  // Client-side parse of "command arg1 arg2..." / "talk npc free text
  // message" into {command, args, message} -- the same split
  // src/mud/mud_http.c's on_mud_command handler expects on the wire,
  // mirroring parse_action_text's own command/args/message split in
  // the guest's own Python-ported logic (mud/guest/mud_guest.cpp).
  function parseCommandLine(raw) {
    const tokens = raw.trim().split(/\s+/).filter(Boolean);
    if (tokens.length === 0) return { command: "look", args: [], message: "" };
    const command = tokens[0].toLowerCase();
    if (command === "talk" && tokens.length > 2) {
      return { command, args: [tokens[1]], message: tokens.slice(2).join(" ") };
    }
    return { command, args: tokens.slice(1), message: "" };
  }

  async function sendCommand(raw) {
    const parsed = parseCommandLine(raw);
    appendTurn("> " + raw);
    try {
      const resp = await fetch("/api/mud/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          session_id: sessionId,
          command: parsed.command,
          args: parsed.args,
          message: parsed.message,
        }),
      });
      const data = await resp.json();
      if (!resp.ok) {
        appendTurn(data.error || ("HTTP " + resp.status), null, true);
        return;
      }
      const meta = "turn " + data.turn + "  " + data.pre_room + " -> " + data.post_room +
        (data.objective_complete ? "  [objective complete]" : "") +
        (data.finished ? "  [session finished]" : "");
      appendTurn(data.narration, meta, data.valid === false);
    } catch (err) {
      appendTurn("connection error: " + err, null, true);
    }
  }

  form.addEventListener("submit", function (ev) {
    ev.preventDefault();
    const raw = input.value;
    input.value = "";
    if (raw.trim()) sendCommand(raw);
  });

  // Live push channel. Task #29 wires the server side's actual content
  // (currently the connection opens and stays open, per
  // src/mud/mud_http.c's own documented on_mud_stream() stub) -- this
  // client side is real and ready for it: any future "data:" frame
  // just gets appended the same way a command's own response does.
  try {
    const stream = new EventSource("/api/mud/stream");
    stream.onmessage = function (ev) {
      appendTurn(ev.data, "live update");
    };
  } catch (err) {
    // EventSource not supported or the connection failed -- the UI
    // still works via plain request/response, so this is not fatal.
  }

  // Real history, not a placeholder: GET /api/mud/history returns every
  // past turn's own narration for this session_id, straight from FDB
  // (src/mud/mud_kv.c's zf/mud/turn/ keyspace). Loaded once on page
  // load, so refreshing the page (or opening the same session_id in a
  // new tab) shows what already happened instead of starting blank.
  async function loadHistory() {
    try {
      const resp = await fetch("/api/mud/history?session_id=" + encodeURIComponent(sessionId));
      if (!resp.ok) return;
      const lines = await resp.json();
      for (const line of lines) appendTurn(line, "history");
    } catch (err) {
      // No FDB configured, or a real network error -- either way the
      // live session still works without history, so this is not fatal.
    }
  }
  loadHistory();

  appendTurn("Connected. Type a command below and press Send.", "session " + sessionId);
})();
