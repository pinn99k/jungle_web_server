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

// 자동으로 다시 하지 않는다. 실패로 보여도 백엔드는 처리했을 수 있어서, 다시 누를지는 사람이 정한다
$("apply").addEventListener("click", async () => {
  const id = $("student").value;
  try {
    const r = await fetch("/api/apply", { method: "POST", body: "student=" + id });
    log(r.ok ? "학번 " + id + " 신청됨 (백엔드 " + (await r.json()).port + ")" : "실패 " + r.status);
  } catch (e) {
    log("연결 끊김. 신청이 됐는지 알 수 없음");
  }
});

loadCourses();
