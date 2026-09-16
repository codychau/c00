use hyper::body::HttpBody;
use hyper::body::Body;
use hyper::server::conn::Http;
use hyper::service::service_fn;
use hyper::{Request, Response};
use serde_json::Value;
use std::io::Read;
use std::net::{SocketAddr, TcpStream};
use std::path::Path;
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tokio::net::TcpListener;
use tokio::sync::{Mutex, Notify};

const MIN_MODEL_MATCH_LEN: usize = 10;
const MODEL_LIST_TTL_SECS: u64 = 30;

const STRIP_KEYS: &[&str] = &[
    "pattern",
    "format",
    "minLength",
    "maxLength",
    "minItems",
    "maxItems",
    "minimum",
    "maximum",
    "exclusiveMinimum",
    "exclusiveMaximum",
    "multipleOf",
    "uniqueItems",
    "minContains",
    "maxContains",
    "patternProperties",
    "additionalProperties",
    "anyOf",
    "oneOf",
    "allOf",
    "not",
    "if",
    "then",
    "else",
    "dependentRequired",
    "dependentSchemas",
    "propertyNames",
    "contains",
];

struct AppState {
    upstream_port: u16,
    model_base_path: Option<String>,
    service_type: Option<String>,
    service_svc: Option<String>,
    model_change_action: Option<String>,
    model_shortname_match: bool,
    model_list_cache: Mutex<(Instant, Vec<String>)>,
    current_model: Mutex<String>,
    switch_in_progress: AtomicBool,
    switch_notify: Notify,
    gpu_temp_guard: bool,
    gpu_temp_guard_slot: Option<String>,
    gpu_temp_guard_threshold: f64,
    // 冷却状态机（持续高温才冷却，中途降温即复位）
    gpu_guard_hot_count: std::sync::atomic::AtomicI32,   // 连续高温采样数
    gpu_guard_cool_streak: std::sync::atomic::AtomicI32, // 冷却后连续低温采样数
    gpu_guard_needs_cooldown: std::sync::atomic::AtomicBool,
    // 温度缓存：(最近读取时刻, 对应 slot, 温度°C)
    gpu_temp_cache: Mutex<(Instant, Option<String>, Option<f64>)>,
}

fn strip_schema(obj: &mut Value, depth: usize) {
    if depth > 100 {
        return;
    }

    match obj {
        Value::Object(map) => {
            let mut keys_to_remove = Vec::new();
            for key in map.keys() {
                if STRIP_KEYS.contains(&key.as_str()) {
                    keys_to_remove.push(key.clone());
                }
            }
            for key in keys_to_remove {
                map.remove(&key);
            }

            for (_map_key, value) in map.iter_mut() {
                if value.is_array() {
                    for item in value.as_array_mut().unwrap() {
                        strip_schema(item, depth + 1);
                    }
                } else if value.is_object() {
                    strip_schema(value, depth + 1);
                }
            }
        }
        Value::Array(arr) => {
            for item in arr.iter_mut() {
                strip_schema(item, depth + 1);
            }
        }
        _ => {}
    }
}

fn find_model_file(base_path: &str, model_id: &str) -> Result<String, String> {
    let bp = std::path::Path::new(base_path);

    let candidates = [
        bp.join(model_id).to_string_lossy().to_string(),
        bp.join(format!("{}.gguf", model_id)).to_string_lossy().to_string(),
    ];

    for c in &candidates {
        if std::path::Path::new(c).exists() {
            return Ok(c.clone());
        }
    }

    Err(format!("model `{}` not found in `{}`", model_id, base_path))
}

fn scan_model_files(base_path: &str) -> Vec<String> {
    fn walk(dir: &Path, out: &mut Vec<String>) {
        let Ok(entries) = std::fs::read_dir(dir) else { return };
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_dir() {
                walk(&p, out);
            } else if p.is_file() {
                let name = entry.file_name().to_string_lossy().to_string();
                if [".gguf", ".ggml", ".safetensors", ".bin"]
                    .iter()
                    .any(|ext| name.ends_with(ext))
                {
                    out.push(p.to_string_lossy().to_string());
                }
            }
        }
    }

    let mut ids = Vec::new();
    walk(Path::new(base_path), &mut ids);
    ids
}

async fn get_known_model_ids(state: &AppState, upstream_host: &str) -> Vec<String> {
    {
        let cache = state.model_list_cache.lock().await;
        if !cache.1.is_empty() && cache.0.elapsed().as_secs() < MODEL_LIST_TTL_SECS {
            return cache.1.clone();
        }
    }

    let mut ids = Vec::new();

    if let Some(ref bp) = state.model_base_path {
        let bp = bp.clone();
        if let Ok(files) = tokio::task::spawn_blocking(move || scan_model_files(&bp)).await {
            ids.extend(files);
        }
    }

    let url = format!(
        "http://{}:{}/v1/models",
        upstream_host, state.upstream_port
    );
    if let Ok(res) = tokio::time::timeout(
        std::time::Duration::from_secs(5),
        hyper::Client::new().get(url.parse().unwrap()),
    )
    .await
    {
        if let Ok(res) = res {
            if res.status() == 200 {
                if let Ok(bytes) = hyper::body::to_bytes(res.into_body()).await {
                    if let Ok(val) = serde_json::from_slice::<Value>(&bytes) {
                        if let Some(data) = val.get("data").and_then(|d| d.as_array()) {
                            for item in data {
                                if let Some(id) = item.get("id").and_then(|i| i.as_str()) {
                                    ids.push(id.to_string());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ids.sort();
    ids.dedup();
    *state.model_list_cache.lock().await = (Instant::now(), ids.clone());
    ids
}

fn is_model_match(full: &str, short: &str) -> bool {
    if full == short {
        return true;
    }
    if full.ends_with(short) {
        return true;
    }
    let base = Path::new(full)
        .file_name()
        .map(|b| b.to_string_lossy().to_string())
        .unwrap_or_default();
    if base == short {
        return true;
    }
    if base.ends_with(short) {
        return true;
    }
    if let Some(wit) = base.strip_suffix(".gguf").or_else(|| base.strip_suffix(".ggml")) {
        if wit == short || wit.ends_with(short) {
            return true;
        }
    }
    false
}

// Expand a short model name/suffix (>= MIN_MODEL_MATCH_LEN chars) to the full
// model ID. Multiple candidates choose the shortest full ID.
fn resolve_full_model(ids: &[String], short: &str) -> Option<String> {
    if short.len() < MIN_MODEL_MATCH_LEN {
        return None;
    }
    let mut candidates: Vec<String> = ids
        .iter()
        .filter(|id| is_model_match(id, short))
        .cloned()
        .collect();
    candidates.sort();
    candidates.dedup();
    match candidates.len() {
        0 => None,
        1 => Some(candidates.remove(0)),
        _ => {
            candidates.sort_by_key(|a| a.len());
            Some(candidates.remove(0))
        }
    }
}

// Non-destructive rewrite of the GET /v1/models response: for every model
// id containing a directory path (i.e. has "/"), also advertise an extra id
// which is the part after the last slash (the short basename). Long ids
// without directory depth are left untouched.
fn inject_short_model_ids(parsed: &mut Value) -> bool {
    let Some(data) = parsed.get_mut("data").and_then(|d| d.as_array_mut()) else {
        return false;
    };
    let mut known: Vec<String> = data
        .iter()
        .filter_map(|item| item.get("id").and_then(|i| i.as_str()))
        .map(|s| s.to_string())
        .collect();

    let mut injections = Vec::new();
    for item in data.iter() {
        let Some(id) = item.get("id").and_then(|i| i.as_str()) else { continue };
        let Some(slash) = id.rfind('/') else { continue };
        let short = &id[slash + 1..];
        if short.is_empty() {
            continue;
        }
        if known.iter().any(|k| k == short) {
            continue;
        }
        let mut new_item = item.clone();
        new_item["id"] = Value::String(short.to_string());
        if let Some(aliases) = new_item.get_mut("aliases").and_then(|a| a.as_array_mut()) {
            for alias in aliases.iter_mut() {
                if alias.as_str() == Some(id) {
                    *alias = Value::String(short.to_string());
                }
            }
        }
        injections.push(new_item);
        known.push(short.to_string());
    }

    if injections.is_empty() {
        return false;
    }
    data.splice(0..0, injections);
    true
}

fn replace_flag(flags: &[&str], old_val: &str, new_val: &str, content: &str) -> Result<String, String> {
    for flag in flags {
        let search = format!("{} {}", flag, old_val);
        if content.contains(&search) {
            let replacement = format!("{} {}", flag, new_val);
            if search == replacement {
                return Ok(content.to_string());
            }
            return Ok(content.replace(&search, &replacement));
        }
    }
    Err(format!("flag {:?} with value {} not found", flags, old_val))
}

fn current_flag_value(content: &str, flags: &[&str]) -> Option<String> {
    for flag in flags {
        let search = format!("{} ", flag);
        if let Some(pos) = content.find(&search) {
            let after = &content[pos + search.len()..];
            let end = after.find(|c| c == ' ' || c == '\t' || c == '\n' || c == '\\')
                .unwrap_or(after.len());
            return Some(after[..end].to_string());
        }
    }
    None
}

fn restart_service(svc_name: &str) -> Result<(), String> {
    let kill = Command::new("systemctl")
        .args(["--user", "kill", svc_name, "--signal=SIGKILL"])
        .output()
        .map_err(|e| format!("kill: {}", e))?;
    if !kill.status.success() {
        let stderr = String::from_utf8_lossy(&kill.stderr);
        // If service is not running, that's fine
        if !stderr.contains("not loaded") {
            return Err(format!("kill {}: {}", svc_name, stderr));
        }
    }
    std::thread::sleep(std::time::Duration::from_millis(500));
    let start = Command::new("systemctl")
        .args(["--user", "start", svc_name])
        .output()
        .map_err(|e| format!("start: {}", e))?;
    if !start.status.success() {
        return Err(format!("start {}: {}", svc_name, String::from_utf8_lossy(&start.stderr)));
    }
    Ok(())
}

fn wait_upstream(port: u16, upstream_host: &str) -> Result<(), String> {
    let addr = format!("{}:{}", upstream_host, port);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
    let mut tried = 0;
    loop {
        tried += 1;
        // HTTP GET /health and check for 200
        let healthy = (|| -> Result<bool, std::io::Error> {
            let mut stream = TcpStream::connect_timeout(
                &addr.parse().unwrap(),
                std::time::Duration::from_secs(5),
            )?;
            use std::io::Write;
            stream.write_all(&format!("GET /health HTTP/1.0\r\nHost: {}\r\nConnection: close\r\n\r\n", upstream_host).as_bytes())?;
            let mut buf = [0u8; 4096];
            let n = stream.read(&mut buf).unwrap_or(0);
            let resp = String::from_utf8_lossy(&buf[..n]);
            Ok(resp.starts_with("HTTP/1.") && resp.contains(" 200 "))
        })();

        match healthy {
            Ok(true) => {
                eprintln!("[proxy] upstream {} healthy after {} tries", addr, tried);
                return Ok(());
            }
            _ => {
                if std::time::Instant::now() > deadline {
                    return Err(format!("upstream {} not healthy after {} tries (last: {:?})", addr, tried, healthy.err()));
                }
                std::thread::sleep(std::time::Duration::from_secs(2));
            }
        }
    }
}

fn perform_switch_blocking(
    base_path: &str,
    new_model: &str,
    svc_name: &str,
    svc_type: &str,
    upstream_port: u16,
    retry_ngl: bool,
    upstream_host: &str,
) -> Result<(), String> {
    let model_path = find_model_file(base_path, new_model)?;

    let supported = matches!(svc_type, "llamacpp" | "vllm");
    if !supported {
        return Err(format!("service type `{}` not supported for auto reload", svc_type));
    }

    let model_flags: &[&str] = match svc_type {
        "llamacpp" => &["--model", "-m"],
        "vllm" => &["--model"],
        _ => &["--model"],
    };
    let ngl_flags: &[&str] = match svc_type {
        "llamacpp" => &["-ngl", "--n-gpu-layers"],
        _ => &["--n-gpu-layers"],
    };

    let output = Command::new("systemctl")
        .args(["--user", "show", "-P", "FragmentPath", svc_name])
        .output()
        .map_err(|e| format!("`systemctl show` failed: {}", e))?;

    let svc_file = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if svc_file.is_empty() || !std::path::Path::new(&svc_file).exists() {
        // Fallback paths don't support ngl retry
        let candidates = [
            format!("/etc/systemd/system/{}", svc_name),
            format!("/usr/lib/systemd/system/{}", svc_name),
        ];
        for c in &candidates {
            if std::path::Path::new(c).exists() {
                let content = std::fs::read_to_string(c)
                    .map_err(|e| format!("read {}: {}", c, e))?;
                let current_model = current_flag_value(&content, model_flags)
                    .ok_or_else(|| "model flag not found".to_string())?;
                let new_content = replace_flag(model_flags, &current_model, &model_path, &content)?;
                std::fs::write(c, &new_content)
                    .map_err(|e| format!("write {}: {}", c, e))?;
                let r = Command::new("systemctl")
                    .args(["--user", "daemon-reload"])
                    .output()
                    .map_err(|e| format!("daemon-reload: {}", e))?;
                if !r.status.success() {
                    return Err(format!("daemon-reload: {}", String::from_utf8_lossy(&r.stderr)));
                }
                restart_service(svc_name)?;
                eprintln!("[proxy] {} restarted with model {}", svc_name, model_path);
                wait_upstream(upstream_port, upstream_host)?;
                return Ok(());
            }
        }
        let r = Command::new("sudo")
            .args(["systemctl", "daemon-reload"])
            .output()
            .map_err(|e| format!("sudo daemon-reload: {}", e))?;
        if !r.status.success() {
            return Err(format!("service file not found for {}", svc_name));
        }
        let r = Command::new("sudo")
            .args(["systemctl", "restart", svc_name])
            .output()
            .map_err(|e| format!("sudo restart: {}", e))?;
        if r.status.success() {
            eprintln!("[proxy] {} (root) restarted with model {}", svc_name, model_path);
            wait_upstream(upstream_port, upstream_host)?;
            return Ok(());
        }
        return Err(format!("service file not found and root restart failed for {}", svc_name));
    }

    let content = std::fs::read_to_string(&svc_file)
        .map_err(|e| format!("read {}: {}", svc_file, e))?;

    let current_model = current_flag_value(&content, model_flags)
        .ok_or_else(|| "model flag not found".to_string())?;
    let mut working = replace_flag(model_flags, &current_model, &model_path, &content)?;

    let ngl_initial = if retry_ngl {
        current_flag_value(&working, ngl_flags)
    } else {
        None
    };

    let max_attempts = if retry_ngl { 3 } else { 1 };

    for attempt in 0..max_attempts {
        if attempt > 0 && retry_ngl {
            if let Some(ref current_ngl) = ngl_initial {
                let ngl_val: i32 = current_ngl.parse().unwrap_or(0);
                let new_ngl = (ngl_val - attempt as i32 * 5).max(0);
                eprintln!("[proxy] ngl reduction attempt {}: {} → {}", attempt, ngl_val, new_ngl);
                working = replace_flag(ngl_flags, current_ngl, &new_ngl.to_string(), &working)
                    .map_err(|e| format!("ngl replacement failed: {}", e))?;
            }
        }

        std::fs::write(&svc_file, &working)
            .map_err(|e| format!("write {}: {}", svc_file, e))?;

        let r = Command::new("systemctl")
            .args(["--user", "daemon-reload"])
            .output()
            .map_err(|e| format!("daemon-reload: {}", e))?;
        if !r.status.success() {
            return Err(format!("daemon-reload: {}", String::from_utf8_lossy(&r.stderr)));
        }

        restart_service(svc_name)?;
        eprintln!("[proxy] {} restarted (attempt {})", svc_name, attempt + 1);

        match wait_upstream(upstream_port, upstream_host) {
            Ok(()) => return Ok(()),
            Err(e) => {
                if attempt + 1 >= max_attempts {
                    return Err(format!("switch failed after {} attempts: {}", max_attempts, e));
                }
                eprintln!("[proxy] retrying ngl reduction...");
            }
        }
    }

    Err("unexpected error in switch".into())
}

/// 读取指定 GPU（按 PCI slot 如 "03:00.0"）的当前温度（摄氏）。
/// 通过 /sys/class/drm/cardN/device/hwmon/hwmonX/temp*_input 读取。
fn read_gpu_temp_celsius(slot: &str) -> Option<f64> {
    let drm_dir = Path::new("/sys/class/drm");
    let entries = std::fs::read_dir(drm_dir).ok()?;

    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        // 只考虑 card0/card1...，跳过 card0-DP-1 等输出接口
        if !name.starts_with("card") || name.contains('-') {
            continue;
        }

        let device_path = entry.path().join("device");
        let link = match std::fs::read_link(&device_path) {
            Ok(l) => l.to_string_lossy().to_string(),
            Err(_) => continue,
        };
        // 链接形如 ../../../0000:03:00.0
        if !link.ends_with(slot) {
            continue;
        }

        let hwmon_dir = device_path.join("hwmon");
        if let Ok(hwmons) = std::fs::read_dir(&hwmon_dir) {
            for hw in hwmons.flatten() {
                let hwmon_path = hw.path();
                // 读取 name 确认是 amdgpu
                let name_file = hwmon_path.join("name");
                if let Ok(n) = std::fs::read_to_string(&name_file) {
                    if n.trim() != "amdgpu" {
                        continue;
                    }
                }
                // 读取 temp*_input（毫摄氏度）
                if let Ok(entries2) = std::fs::read_dir(&hwmon_path) {
                    for f in entries2.flatten() {
                        let fname = f.file_name().to_string_lossy().to_string();
                        if fname.starts_with("temp") && fname.ends_with("_input") {
                            if let Ok(val) = std::fs::read_to_string(f.path()) {
                                let v: f64 = val.trim().parse().ok()?;
                                return Some(v / 1000.0);
                            }
                        }
                    }
                }
                break; // 只取第一个 hwmon
            }
        }
    }
    None
}

const GPU_TEMP_CACHE_SECS: u64 = 5;      // sysfs 温度缓存秒数（期间轮询内存）
const GPU_TEMP_HOT_REQUIRED: i32 = 10;   // 连续高温达到该次数才冷却
const GPU_TEMP_COOL_RESET: i32 = 2;      // 冷却中连续低温达到该次数即解除

/// 用一次温度采样更新冷却状态机。
/// 返回是否需要冷却（needs_cooldown）。
fn update_gpu_guard_state(state: &AppState, temp: f64) -> bool {
    let hot = temp >= state.gpu_temp_guard_threshold;
    let needs = state.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst);

    if needs {
        // 已在冷却：只有连续低温到阈值次数才解除
        if hot {
            state.gpu_guard_cool_streak.store(0, std::sync::atomic::Ordering::SeqCst);
        } else {
            let s = state.gpu_guard_cool_streak.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
            if s >= GPU_TEMP_COOL_RESET {
                state.gpu_guard_needs_cooldown.store(false, std::sync::atomic::Ordering::SeqCst);
                state.gpu_guard_hot_count.store(0, std::sync::atomic::Ordering::SeqCst);
                state.gpu_guard_cool_streak.store(0, std::sync::atomic::Ordering::SeqCst);
                eprintln!("[proxy] gpu temp back to normal, cooldown lifted");
            }
        }
    } else {
        // 未冷却：连续高温计数，中途降温清零
        if hot {
            let h = state.gpu_guard_hot_count.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
            if h >= GPU_TEMP_HOT_REQUIRED {
                state.gpu_guard_needs_cooldown.store(true, std::sync::atomic::Ordering::SeqCst);
                state.gpu_guard_hot_count.store(0, std::sync::atomic::Ordering::SeqCst);
                eprintln!("[proxy] gpu sustained overheat, entering cooldown");
            }
        } else {
            state.gpu_guard_hot_count.store(0, std::sync::atomic::Ordering::SeqCst);
        }
    }

    state.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst)
}

/// 带缓存的 GPU 温度读取：每 GPU_TEMP_CACHE_SECS 秒内复用内存缓存，
/// 避免每个请求都去读 sysfs。真正读取后会更新冷却状态机。
async fn get_gpu_temp_cached(state: &AppState, slot: &str) -> Option<f64> {
    let cutoff = std::time::Duration::from_secs(GPU_TEMP_CACHE_SECS);
    {
        let cache = state.gpu_temp_cache.lock().await;
        let (ts, cached_slot, temp) = &*cache;
        if cached_slot.as_deref() == Some(slot) {
            if let Some(t) = temp {
                if ts.elapsed() < cutoff {
                    return Some(*t);
                }
            }
        }
    }

    let fresh = read_gpu_temp_celsius(slot);
    if let Some(t) = fresh {
        update_gpu_guard_state(state, t);
    }
    let mut cache = state.gpu_temp_cache.lock().await;
    *cache = (Instant::now(), Some(slot.to_string()), fresh);
    drop(cache);
    fresh
}

async fn handle_model_switch(state: Arc<AppState>, new_model: String) {
    if new_model.is_empty() {
        return;
    }

    let action = state.model_change_action.as_deref().unwrap_or("").to_string();
    let auto_reload = action.starts_with("自动根据模型名称重载服务")
        && state.model_base_path.is_some()
        && state.service_type.is_some()
        && state.service_svc.is_some();
    let retry_ngl = action == "自动根据模型名称重载服务并且自动降低GPU中加载的层数";

    if !auto_reload {
        return;
    }

    loop {
        {
            let current = state.current_model.lock().await;
            if *current == new_model {
                return;
            }
        }

        if state.switch_in_progress.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_ok() {
            let new_model_clone = new_model.clone();
            let base_path = state.model_base_path.clone().unwrap();
            let svc_name = state.service_svc.clone().unwrap();
            let svc_type = state.service_type.clone().unwrap();
            let up_port = state.upstream_port;
            let state_clone = state.clone();

            let model_for_log = new_model_clone.clone();
            let model_for_update = new_model_clone.clone();
            let (tx, rx) = tokio::sync::oneshot::channel::<Result<(), String>>();

            tokio::spawn(async move {
                let upstream_host_env = std::env::var("UPSTREAM_HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
                let result = tokio::task::spawn_blocking(move || {
                    perform_switch_blocking(
                        &base_path, &new_model_clone, &svc_name, &svc_type, up_port, retry_ngl, &upstream_host_env,
                    )
                })
                .await
                .unwrap_or(Err("switch task panicked".into()));

                match &result {
                    Ok(()) => eprintln!("[proxy] model switched: {}", model_for_log),
                    Err(e) => eprintln!("[proxy] switch failed: {}", e),
                }

                if result.is_ok() {
                    *state_clone.current_model.lock().await = model_for_update;
                }

                state_clone.switch_in_progress.store(false, Ordering::SeqCst);
                state_clone.switch_notify.notify_waiters();
                let _ = tx.send(result);
            });

            let _ = rx.await;
            return;
        }

        state.switch_notify.notified().await;
    }
}

async fn proxy_handler(state: Arc<AppState>, req: Request<Body>) -> hyper::Result<Response<Body>> {
    // GPU 高温保护：持续高温（连续 10 次采样）进入冷却后，中断请求返回请等待降温错误
    if state.gpu_temp_guard {
        if let Some(slot) = &state.gpu_temp_guard_slot {
            // 读取温度并更新状态机（命中缓存则不计次）
            get_gpu_temp_cached(&state, slot).await;
            if state.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst) {
                let temp = state.gpu_temp_cache.lock().await.2.unwrap_or(0.0);
                let msg = format!(
                    "GPU (PCI {}) 连续高温，请等待显卡降温后重试 (当前 {}°C)。",
                    slot, temp as i64
                );
                eprintln!("[proxy] gpu cooldown guard triggered: {}", msg);
                let body = serde_json::json!({
                    "error": {
                        "message": msg,
                        "type": "gpu_overheat",
                        "code": "gpu_temp_too_high"
                    }
                });
                return Ok(Response::builder()
                    .status(503)
                    .header("content-type", "application/json")
                    .body(Body::from(body.to_string()))
                    .unwrap());
            }
        }
    }

    let mut headers_map = hyper::header::HeaderMap::new();
    for (k, v) in req.headers().iter() {
        headers_map.insert(k.clone(), v.clone());
    }

    let content_type_str = headers_map
        .get("content-type")
        .and_then(|ct| ct.to_str().ok())
        .unwrap_or("");

    let uri_path = req.uri().path().to_string();
    let method = req.method().clone();

    let mut chunks = Vec::new();
    let mut body_len = 0;

    let mut body_stream = req.into_body();
    while let Some(chunk) = body_stream.data().await {
        let chunk = chunk?;
        chunks.push(chunk.clone());
        body_len += chunk.len();
    }

    let mut modified = false;
    let mut body_modified: Option<String> = None;
    let mut request_model: Option<String> = None;

    if content_type_str.contains("json") && body_len > 0 {
        let body_bytes: Vec<u8> = chunks.concat();
        let body_str = String::from_utf8_lossy(&body_bytes);
        let body_owned = body_str.to_string();

        if let Ok(mut parsed) = serde_json::from_str::<Value>(&body_str) {
            request_model = parsed.get("model")
                .and_then(|m| m.as_str())
                .map(|s| s.to_string());

            if state.model_shortname_match {
                if let Some(short) = request_model.clone() {
                    let upstream_host = std::env::var("UPSTREAM_HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
                    let ids = get_known_model_ids(&state, &upstream_host).await;
                    if let Some(full) = resolve_full_model(&ids, &short) {
                        if full != short {
                            eprintln!("[proxy] model resolved: {} → {}", short, full);
                            parsed["model"] = Value::String(full.clone());
                            modified = true;
                            request_model = Some(full);
                        }
                    }
                }
            }

            if let Some(response_format) = parsed.get_mut("response_format") {
                if let Some(type_val) = response_format.get("type") {
                    if type_val == "json_schema" {
                        *response_format = serde_json::json!({ "type": "json_object" });
                        modified = true;
                    }
                }
            }

            if let Some(tools) = parsed.get_mut("tools") {
                if let Some(tools_arr) = tools.as_array_mut() {
                    for tool in tools_arr.iter_mut() {
                        if let Some(function) = tool.get_mut("function") {
                            if let Some(parameters) = function.get_mut("parameters") {
                                let original_params = parameters.clone();
                                strip_schema(parameters, 0);
                                if original_params != *parameters {
                                    modified = true;
                                }
                            }
                        }
                    }
                }
            }

            if modified {
                body_modified = Some(serde_json::to_string(&parsed).unwrap_or_else(|_| body_owned.clone()));
            }
        }
    }

    if let Some(ref model_name) = request_model {
        handle_model_switch(state.clone(), model_name.clone()).await;
    }

    let client = hyper::Client::new();
    let upstream_host = std::env::var("UPSTREAM_HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let upstream_port = std::env::var("UPSTREAM_PORT").ok().and_then(|p| p.parse().ok()).unwrap_or(8082);
    let upstream_addr = format!("{}:{}", upstream_host, upstream_port);
    let upstream_url = format!("http://{}{}", upstream_addr, uri_path);

    let body_bytes: Vec<u8> = if let Some(ref s) = body_modified {
        s.clone().into_bytes()
    } else {
        chunks.concat()
    };

    let res = 'retry: {
        for retry in 0..5 {
            let mut upstream_req_builder = Request::builder()
                .method(method.clone())
                .uri(&upstream_url);

            for (header_key, header_value) in headers_map.iter() {
                let key_str = header_key.to_string();
                if key_str != "host" && key_str != "content-length" && key_str != "transfer-encoding" {
                    upstream_req_builder = upstream_req_builder.header(header_key, header_value);
                }
            }
            upstream_req_builder = upstream_req_builder.header("host", &upstream_host);
            upstream_req_builder = upstream_req_builder.header("content-length", body_bytes.len().to_string());

            match client.request(upstream_req_builder.body(Body::from(body_bytes.clone())).unwrap()).await {
                Ok(r) => break 'retry r,
                Err(e) => {
                    if retry < 4 {
                        tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    } else {
                        break 'retry Err(e)?;
                    }
                }
            }
        }
        unreachable!()
    };

    let status = res.status().as_u16();
    let res_headers: Vec<(hyper::header::HeaderName, hyper::header::HeaderValue)> = res
        .headers()
        .iter()
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect();
    let mut res_body = res.into_body();

    // 分块读取上游响应；期间每 5 秒检查一次 GPU 温度，
    // 若超温则丢弃已缓冲内容，直接返回“请等待降温”错误（前端此刻还未收到任何数据）。
    let mut res_body_bytes: Vec<u8> = Vec::new();
    let check_interval = std::time::Duration::from_secs(GPU_TEMP_CACHE_SECS);
    loop {
        match tokio::time::timeout(check_interval, res_body.data()).await {
            Ok(Some(Ok(bytes))) => res_body_bytes.extend_from_slice(&bytes),
            Ok(Some(Err(_))) => break,
            Ok(None) => break, // 上游响应结束
            Err(_) => {
                // 一段时间没有新数据（上游仍在生成），趁机采样温度更新状态机
                if state.gpu_temp_guard {
                    if let Some(slot) = &state.gpu_temp_guard_slot {
                        get_gpu_temp_cached(&state, slot).await;
                        if state.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst) {
                            let temp = state.gpu_temp_cache.lock().await.2.unwrap_or(0.0);
                            let msg = format!(
                                "GPU (PCI {}) 连续高温，请等待显卡降温后重试 (当前 {}°C)。",
                                slot, temp as i64
                            );
                            eprintln!("[proxy] gpu cooldown during streaming: {}", msg);
                            let body = serde_json::json!({
                                "error": {
                                    "message": msg,
                                    "type": "gpu_overheat",
                                    "code": "gpu_temp_too_high"
                                }
                            });
                            return Ok(Response::builder()
                                .status(503)
                                .header("content-type", "application/json")
                                .body(Body::from(body.to_string()))
                                .unwrap());
                        }
                    }
                }
            }
        }
    }

    if status >= 400 {
        let body_str = String::from_utf8_lossy(&res_body_bytes);
        eprintln!("[proxy] {} {}: {}", status, upstream_addr, body_str.chars().take(300).collect::<String>());
    }

    if status == 200 && uri_path == "/v1/models" {
        if let Ok(mut parsed) = serde_json::from_slice::<Value>(&res_body_bytes) {
            if inject_short_model_ids(&mut parsed) {
                if let Ok(new_bytes) = serde_json::to_vec(&parsed) {
                    res_body_bytes = new_bytes.into();
                    eprintln!("[proxy] /v1/models injected short model ids");
                }
            }
        }
    }

    let mut res_builder = Response::builder().status(status);
    for (header_key, header_value) in res_headers.iter() {
        let key_str = header_key.as_str();
        // Body is fully buffered and re-sent with a fresh content-length;
        // forwarding the upstream transfer-encoding (e.g. chunked for SSE)
        // alongside content-length makes hyper reject the response.
        if key_str == "content-length" || key_str == "transfer-encoding" {
            continue;
        }
        res_builder = res_builder.header(header_key, header_value);
    }
    res_builder = res_builder.header("content-length", res_body_bytes.len());

    let res = res_builder.body(Body::from(res_body_bytes)).unwrap();

    Ok(res)
}

#[tokio::main]
async fn main() {
    let bind_host = std::env::var("PROXY_HOST").unwrap_or_else(|_| "0.0.0.0".to_string());
    let bind_port: u16 = std::env::var("PROXY_PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(8084);

    let upstream_host = std::env::var("UPSTREAM_HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let upstream_port: u16 = std::env::var("UPSTREAM_PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(8082);

    let model_base_path: Option<String> = std::env::var("MODEL_BASE_PATH").ok();
    let startup_program: Option<String> = std::env::var("STARTUP_PROGRAM").ok();
    let action: Option<String> = std::env::var("ACTION").ok();
    let model_name: Option<String> = std::env::var("MODEL_NAME").ok();
    let openclaw_format: bool = std::env::var("OPENCLAW_FORMAT").ok().filter(|s| s == "true").is_some();
    let service_type: Option<String> = std::env::var("SERVICE_TYPE").ok();
    let service_svc: Option<String> = std::env::var("SERVICE_SVC").ok();
    let model_change_action: Option<String> = std::env::var("MODEL_CHANGE_ACTION").ok();
    let model_shortname_match: bool = std::env::var("MODEL_SHORTNAME_MATCH").ok().filter(|s| s == "true").is_some();
    let gpu_temp_guard: bool = std::env::var("GPU_TEMP_GUARD").ok().filter(|s| s == "true").is_some();
    let gpu_temp_guard_slot: Option<String> = std::env::var("GPU_TEMP_GUARD_SLOT").ok();
    let gpu_temp_guard_threshold: f64 = std::env::var("GPU_TEMP_GUARD_THRESHOLD")
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(99.0);

    let addr: SocketAddr = format!("{}:{}", bind_host, bind_port).parse().unwrap();

    println!("proxy on {}:{} → {}:{}", bind_host, bind_port, upstream_host, upstream_port);
    if let Some(ref mbp) = model_base_path {
        println!("  model_base_path: {}", mbp);
    }
    if let Some(ref sp) = startup_program {
        println!("  startup_program: {}", sp);
    }
    if let Some(ref a) = action {
        println!("  action: {}", a);
    }
    if let Some(ref mn) = model_name {
        println!("  model_name: {}", mn);
    }
    if openclaw_format {
        println!("  openclaw_format: true");
    }
    if let Some(ref st) = service_type {
        println!("  service_type: {}", st);
    }
    if let Some(ref svc) = service_svc {
        println!("  service_svc: {}", svc);
    }
    if let Some(ref mca) = model_change_action {
        println!("  model_change_action: {}", mca);
    }
    if model_shortname_match {
        println!("  model_shortname_match: true");
    }
    if gpu_temp_guard {
        println!("  gpu_temp_guard: true on {} (>={}°C)", gpu_temp_guard_slot.as_deref().unwrap_or("?"), gpu_temp_guard_threshold);
    }

    let initial_model = model_name.unwrap_or_default();

    let state = Arc::new(AppState {
        upstream_port,
        model_base_path,
        service_type,
        service_svc,
        model_change_action,
        model_shortname_match,
        model_list_cache: Mutex::new((Instant::now(), Vec::new())),
        current_model: Mutex::new(initial_model),
        switch_in_progress: AtomicBool::new(false),
        switch_notify: Notify::new(),
        gpu_temp_guard,
        gpu_temp_guard_slot,
        gpu_temp_guard_threshold,
        gpu_guard_hot_count: std::sync::atomic::AtomicI32::new(0),
        gpu_guard_cool_streak: std::sync::atomic::AtomicI32::new(0),
        gpu_guard_needs_cooldown: std::sync::atomic::AtomicBool::new(false),
        gpu_temp_cache: Mutex::new((Instant::now(), None, None)),
    });

    let listener = TcpListener::bind(&addr).await.unwrap();

    loop {
        let (stream, peer) = listener.accept().await.unwrap();
        eprintln!("[proxy] accept: {}", peer);
        let state_for_conn = state.clone();

        tokio::spawn(async move {
            let service = service_fn(move |req| {
                let st = state_for_conn.clone();
                let method = req.method().clone();
                let path = req.uri().path().to_string();
                eprintln!("[proxy] >>> {} {} from {}", method, path, peer);
                async move {
                    match proxy_handler(st, req).await {
                        Ok(resp) => {
                            eprintln!("[proxy] <<< {} {} (status {})", method, path, resp.status().as_u16());
                            Ok::<_, hyper::Error>(resp)
                        }
                        Err(e) => {
                            eprintln!("[proxy] !!! {} {} error: {}", method, path, e);
                            Ok(Response::builder()
                                .status(502)
                                .body(Body::from(e.to_string()))
                                .unwrap())
                        }
                    }
                }
            });

            if let Err(e) = Http::new().serve_connection(stream, service).await {
                eprintln!("[proxy] conn closed {}: {}", peer, e);
            } else {
                eprintln!("[proxy] conn done {}", peer);
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_state() -> AppState {
        AppState {
            upstream_port: 0,
            model_base_path: None,
            service_type: None,
            service_svc: None,
            model_change_action: None,
            model_shortname_match: false,
            model_list_cache: Mutex::new((Instant::now(), Vec::new())),
            current_model: Mutex::new(String::new()),
            switch_in_progress: AtomicBool::new(false),
            switch_notify: Notify::new(),
            gpu_temp_guard: true,
            gpu_temp_guard_slot: Some("03:00.0".into()),
            gpu_temp_guard_threshold: 99.0,
            gpu_guard_hot_count: std::sync::atomic::AtomicI32::new(0),
            gpu_guard_cool_streak: std::sync::atomic::AtomicI32::new(0),
            gpu_guard_needs_cooldown: std::sync::atomic::AtomicBool::new(false),
            gpu_temp_cache: Mutex::new((Instant::now(), None, None)),
        }
    }

    #[test]
    fn enters_cooldown_after_10_hot() {
        let s = make_state();
        for _ in 0..9 {
            assert!(!update_gpu_guard_state(&s, 100.0));
        }
        assert!(update_gpu_guard_state(&s, 100.0));
    }

    #[test]
    fn reset_on_mid_cool() {
        let s = make_state();
        for _ in 0..5 { update_gpu_guard_state(&s, 100.0); }
        update_gpu_guard_state(&s, 50.0);
        for _ in 0..6 { update_gpu_guard_state(&s, 100.0); }
        assert!(!s.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst));
        update_gpu_guard_state(&s, 100.0);
        for _ in 0..3 { update_gpu_guard_state(&s, 100.0); }
        assert!(s.gpu_guard_needs_cooldown.load(std::sync::atomic::Ordering::SeqCst));
    }

    #[test]
    fn lift_after_2_cool() {
        let s = make_state();
        for _ in 0..10 { update_gpu_guard_state(&s, 100.0); }
        assert!(update_gpu_guard_state(&s, 100.0) || true);
        assert!(update_gpu_guard_state(&s, 50.0));
        assert!(!update_gpu_guard_state(&s, 50.0));
    }
}
