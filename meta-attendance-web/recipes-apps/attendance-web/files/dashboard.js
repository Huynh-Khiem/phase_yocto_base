"use strict";

const processLabels = {
    "attendance-system": "Điều phối",
    "pir-test": "Cảm biến PIR",
    "tapo-rtsp": "Camera RTSP",
    "attendance-ai": "Nhận diện AI",
    "tft-test": "Màn hình TFT",
};

const resultLabels = {
    "match": ["Đã khớp", "good"],
    "unknown": ["Không khớp", "warn"],
    "no-face": ["Không thấy mặt", "neutral"],
    "no-reference": ["Chưa có mẫu", "neutral"],
    "timeout": ["Hết thời gian", "warn"],
    "camera-error": ["Lỗi camera", "error"],
    "ai-error": ["Lỗi AI", "error"],
};

const dateFormat = new Intl.DateTimeFormat("vi-VN", {
    timeZone: "Asia/Ho_Chi_Minh",
    dateStyle: "short",
    timeStyle: "medium",
});
const vietnamDate = new Intl.DateTimeFormat("sv-SE", { timeZone: "Asia/Ho_Chi_Minh" });

const processList = document.getElementById("process-list");
const processMessage = document.getElementById("process-message");
const processCount = document.getElementById("process-count");
const resultWrap = document.getElementById("results-wrap");
const resultBody = document.getElementById("results-body");
const resultMessage = document.getElementById("results-message");
const attendanceDate = document.getElementById("attendance-date");
const attendanceCount = document.getElementById("attendance-count");
const attendanceWrap = document.getElementById("attendance-wrap");
const attendanceBody = document.getElementById("attendance-body");
const attendanceMessage = document.getElementById("attendance-message");
const connection = document.getElementById("connection");
let followToday = true;
let attendanceRequest = 0;

function element(tag, text, className) {
    const node = document.createElement(tag);
    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;
    return node;
}

function badge(text, tone) {
    return element("span", text, `badge badge-${tone}`);
}

function timeCell(finishedMs) {
    const date = new Date(finishedMs);
    const time = element("time", dateFormat.format(date));
    time.dateTime = date.toISOString();
    const cell = element("td");
    cell.append(time);
    return cell;
}

function showProcesses(processes) {
    const count = processes.filter(({ pids }) => pids.length > 0).length;
    const controllerRunning = processes.some(({ name, pids }) =>
        name === "attendance-system" && pids.length > 0);

    processCount.textContent = `${count}/${processes.length} đang chạy`;
    processList.replaceChildren(...processes.map(({ name, pids }) => {
        const running = pids.length > 0;
        const onDemand = ["tapo-rtsp", "attendance-ai", "tft-test"].includes(name);
        const status = running ? "Đang chạy" : controllerRunning && onDemand ? "Đang chờ" : "Đã dừng";
        const detail = running ? `${name} · PID ${pids.join(", ")}` : name;
        const info = element("div", undefined, "process-info");
        info.append(element("span", processLabels[name] || name, "process-name"),
                    element("span", detail, "process-detail"));
        const row = element("li");
        row.append(info, badge(status, running ? "good" : "neutral"));
        return row;
    }));
    processMessage.hidden = true;
    processList.hidden = false;
}

function showResults(results) {
    if (!results.length) {
        resultWrap.hidden = true;
        resultMessage.textContent = "Chưa có lượt nhận diện.";
        resultMessage.hidden = false;
        return;
    }

    resultBody.replaceChildren(...results.map(({ finished_ms, status, person_id, cosine }) => {
        const [label, tone] = resultLabels[status] || [status, "neutral"];
        const row = element("tr");
        const statusCell = element("td");
        statusCell.append(badge(label, tone));
        row.append(timeCell(finished_ms), statusCell, element("td", person_id ?? "—", "person"),
                   element("td", cosine == null ? "—" : cosine.toFixed(3), "score"));
        return row;
    }));
    resultMessage.hidden = true;
    resultWrap.hidden = false;
}

async function read(url) {
    const signal = AbortSignal.timeout(3500);
    const response = await fetch(url, { cache: "no-store", signal });
    if (!response.ok) throw new Error(`${url}: ${response.status}`);
    return response.json();
}

async function updateAttendance() {
    if (followToday) attendanceDate.value = vietnamDate.format(new Date());
    const request = ++attendanceRequest;
    try {
        const rows = await read("/api/attendance?date=" + encodeURIComponent(attendanceDate.value));
        if (request !== attendanceRequest) return;
        attendanceCount.textContent = `${rows.length} người`;
        attendanceBody.replaceChildren(...rows.map(({ person_id, finished_ms, cosine }) => {
            const row = element("tr");
            row.append(element("td", person_id ?? "—", "person"), timeCell(finished_ms),
                       element("td", cosine == null ? "—" : cosine.toFixed(3), "score"));
            return row;
        }));
        attendanceMessage.textContent = "Chưa có ai điểm danh trong ngày này.";
        attendanceMessage.hidden = rows.length > 0;
        attendanceWrap.hidden = rows.length === 0;
    } catch {
        if (request !== attendanceRequest) return;
        attendanceCount.textContent = "";
        attendanceWrap.hidden = true;
        attendanceMessage.textContent = "Chưa đọc được danh sách điểm danh.";
        attendanceMessage.hidden = false;
    }
}

attendanceDate.addEventListener("change", () => {
    followToday = !attendanceDate.value || attendanceDate.value === vietnamDate.format(new Date());
    attendanceCount.textContent = "";
    attendanceWrap.hidden = true;
    attendanceMessage.textContent = "Đang đọc danh sách…";
    attendanceMessage.hidden = false;
    updateAttendance();
});

async function update() {
    if (!document.hidden) {
        const [processes, results] = await Promise.allSettled([
            read("/api/status").then(showProcesses),
            read("/api/results").then(showResults),
            updateAttendance(),
        ]);

        if (processes.status === "rejected") {
            processList.hidden = true;
            processCount.textContent = "";
            processMessage.textContent = "Chưa đọc được trạng thái tiến trình.";
            processMessage.hidden = false;
        }
        if (results.status === "rejected") {
            resultWrap.hidden = true;
            resultMessage.textContent = "Chưa đọc được kết quả nhận diện.";
            resultMessage.hidden = false;
        }

        const connected = processes.status === "fulfilled" || results.status === "fulfilled";
        const newText = connected ? "Đã kết nối" : "Mất kết nối";
        if (connection.textContent !== newText) {
            connection.textContent = newText;
            connection.className = `badge badge-${connected ? "good" : "error"}`;
        }
    }
    setTimeout(update, 2000);
}

update();
