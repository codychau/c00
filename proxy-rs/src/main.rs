use hyper::body::HttpBody;
use hyper::body::Body;
use hyper::server::conn::Http;
use hyper::service::service_fn;
use hyper::{Request, Response};
use serde_json::Value;
use std::io::Read;
use std::net::{SocketAddr, TcpStream};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tokio::net::TcpListener;
use tokio::sync::{Mutex, Notify};

const UPSTREAM_HOST: &str = "127.0.0.1";

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
    current_model: Mutex<String>,
    switch_in_progress: AtomicBool,
    switch_notify: Notify,
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

fn wait_upstream(port: u16) -> Result<(), String> {
    let addr = format!("127.0.0.1:{}", port);
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
            stream.write_all(b"GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")?;
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
                wait_upstream(upstream_port)?;
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
            wait_upstream(upstream_port)?;
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

        match wait_upstream(upstream_port) {
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
                let result = tokio::task::spawn_blocking(move || {
                    perform_switch_blocking(
                        &base_path, &new_model_clone, &svc_name, &svc_type, up_port, retry_ngl,
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
    let upstream_addr = format!("{}:{}", UPSTREAM_HOST, state.upstream_port);
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
                .uri(&upstream_url)
                .header("host", UPSTREAM_HOST);

            for (header_key, header_value) in headers_map.iter() {
                let key_str = header_key.to_string();
                if key_str != "host" && key_str != "content-length" && key_str != "transfer-encoding" {
                    upstream_req_builder = upstream_req_builder.header(header_key, header_value);
                }
            }
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
    let res_body_bytes = hyper::body::to_bytes(res.into_body()).await.unwrap_or_default();

    if status >= 400 {
        let body_str = String::from_utf8_lossy(&res_body_bytes);
        eprintln!("[proxy] {} {}: {}", status, upstream_addr, body_str.chars().take(300).collect::<String>());
    }

    let mut res_builder = Response::builder().status(status);
    for (header_key, header_value) in res_headers.iter() {
        res_builder = res_builder.header(header_key, header_value);
    }

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

    let addr: SocketAddr = format!("{}:{}", bind_host, bind_port).parse().unwrap();

    println!("proxy on {}:{} → {}:{}", bind_host, bind_port, UPSTREAM_HOST, upstream_port);
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

    let initial_model = model_name.unwrap_or_default();

    let state = Arc::new(AppState {
        upstream_port,
        model_base_path,
        service_type,
        service_svc,
        model_change_action,
        current_model: Mutex::new(initial_model),
        switch_in_progress: AtomicBool::new(false),
        switch_notify: Notify::new(),
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
