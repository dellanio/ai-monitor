// AI Monitor v0.1 — implementação do modelo de dados.
#include "model.h"
#include "config.h"

SessionStatus statusFromString(const String& s) {
  String v = s;
  v.toLowerCase();
  v.trim();
  if (v == "needs-you" || v == "needs_you" || v == "waiting" ||
      v == "precisa" || v == "approval" || v == "needsyou") {
    return ST_NEEDYOU;
  }
  if (v == "finished" || v == "done" || v == "complete" ||
      v == "terminou" || v == "completed") {
    return ST_FINISHED;
  }
  if (v == "working" || v == "streaming" || v == "starting" ||
      v == "resuming" || v == "running" || v == "active") {
    return ST_WORKING;
  }
  return ST_IDLE;
}

const char* statusLabel(SessionStatus st) {
  switch (st) {
    case ST_WORKING: return "trabalhando";
    case ST_NEEDYOU: return "precisa de voce";
    case ST_FINISHED: return "terminou";
    default: return "parado";
  }
}

uint16_t statusColor(SessionStatus st) {
  switch (st) {
    case ST_WORKING: return COL_WORKING;
    case ST_NEEDYOU: return COL_NEEDYOU;
    case ST_FINISHED: return COL_FINISHED;
    default: return COL_IDLE;
  }
}

Session* findSession(AppState& st, const String& id) {
  for (int i = 0; i < st.sessionCount; i++) {
    if (st.sessions[i].id == id) return &st.sessions[i];
  }
  return nullptr;
}

void upsertSession(AppState& st, const Session& s) {
  Session* cur = findSession(st, s.id);
  if (cur) {
    SessionStatus old = cur->status;
    *cur = s;
    cur->updatedAt = millis();
    if (s.status == ST_FINISHED && old != ST_FINISHED) {
      st.lastFinishedTitle = s.title;
      st.lastFinishedMs = millis();
    }
    if (s.status != old) st.lastActivityMs = millis();
    return;
  }
  if (st.sessionCount < MAX_SESSIONS) {
    st.sessions[st.sessionCount] = s;
    st.sessions[st.sessionCount].updatedAt = millis();
    if (s.status == ST_FINISHED) {
      st.lastFinishedTitle = s.title;
      st.lastFinishedMs = millis();
    }
    st.lastActivityMs = millis();
    st.sessionCount++;
  }
}

void clearFinishedOld(AppState& st) {
  // Remove sessões terminadas há mais de 10 min, para a lista não crescer
  // para sempre. (v0.1: só limpa as terminadas; as ativas ficam.)
  uint32_t now = millis();
  int w = 0;
  for (int r = 0; r < st.sessionCount; r++) {
    bool drop = (st.sessions[r].status == ST_FINISHED) &&
                (now - st.sessions[r].updatedAt > 10UL * 60UL * 1000UL);
    if (!drop) st.sessions[w++] = st.sessions[r];
  }
  st.sessionCount = w;
}

void replaceSessions(AppState& st, const Session* incoming, int n) {
  // O frame "sessions" do Hub é um retrato completo: o que não veio, saiu.
  static Session next[MAX_SESSIONS];
  int w = 0;
  uint32_t now = millis();
  bool quiet = st.snapshots == 0;  // 1o retrato após conectar: sem som
  for (int i = 0; i < n && w < MAX_SESSIONS; i++) {
    const Session& s = incoming[i];
    if (s.id.length() == 0) continue;
    Session* old = findSession(st, s.id);
    next[w] = s;
    next[w].updatedAt = now;
    if (old) {
      if (old->status == s.status) next[w].updatedAt = old->updatedAt;
      if (s.status == ST_FINISHED && old->status != ST_FINISHED) {
        st.lastFinishedTitle = s.title;
        st.lastFinishedMs = now;
      }
      if (old->status != s.status) st.lastActivityMs = now;
      if (!quiet && s.status == ST_NEEDYOU && old->status != ST_NEEDYOU) st.sndNeedYou++;
    } else {
      st.lastActivityMs = now;
      if (!quiet) {
        if (s.status == ST_NEEDYOU) st.sndNeedYou++;
        else if (s.status == ST_WORKING) st.sndNew++;
      }
    }
    w++;
  }
  for (int i = 0; i < w; i++) st.sessions[i] = next[i];
  st.sessionCount = w;
  st.snapshots++;
  if (st.selectedSession >= w) st.selectedSession = -1;
}

int countStatus(const AppState& st, SessionStatus s) {
  int n = 0;
  for (int i = 0; i < st.sessionCount; i++)
    if (st.sessions[i].status == s) n++;
  return n;
}
