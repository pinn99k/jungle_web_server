const $ = (id) => document.getElementById(id);

// 결과 한 줄을 목록 맨 위에 더한다
function log(text) {
  const li = document.createElement("li");
  li.textContent = new Date().toLocaleTimeString() + " " + text;
  $("log").prepend(li);
}

async function loadCourses() {
  try {
    const r = await fetch("/api/courses");
    if (!r.ok) throw new Error(r.status);
    const body = await r.json();
    $("courses").replaceChildren(...body.courses.map((name) => {
      const li = document.createElement("li");
      li.textContent = name;
      return li;
    }));
    log("과목 목록을 " + body.port + "번 백엔드가 줌");
  } catch (e) {
    log("과목 목록 실패: " + e.message);
  }
}

// 줄을 서는 중이면 서버가 준 번호표(쿠키)로 기다린다. 다시 눌러도 순서는 그대로다
$("apply").addEventListener("click", async () => {
  const id = $("student").value;
  try {
    for (;;) {
      const r = await fetch("/api/apply", { method: "POST", body: "student=" + id });
      const body = r.headers.get("Content-Type") === "application/json" ? await r.json() : {};
      if (r.status === 503 && "ahead" in body) {
        log("내 앞에 " + body.ahead + "명");
        await new Promise((done) => setTimeout(done, body.wait_ms || 1000));
        continue;
      }
      log(r.ok ? "학번 " + id + (body.already ? " 이미 신청됨" : " 신청됨 (백엔드 " + body.port + ")") : "실패 " + r.status);
      break;
    }
  } catch (e) {
    log("연결 끊김. 신청이 됐는지 알 수 없음");
  }
});

loadCourses();
