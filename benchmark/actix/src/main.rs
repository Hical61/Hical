use actix_web::{web, App, HttpServer, HttpResponse, Responder};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
struct UserDTO {
    name: String,
    age: i32,
    email: String,
}

#[derive(Serialize)]
struct StatusResponse {
    status: &'static str,
    framework: &'static str,
}

#[derive(Serialize)]
struct UserResponse {
    #[serde(rename = "userId")]
    user_id: String,
    name: String,
}

// Hello World
async fn hello() -> impl Responder {
    "Hello, World!"
}

// JSON 响应
async fn status() -> impl Responder {
    HttpResponse::Ok().json(StatusResponse {
        status: "running",
        framework: "actix-web",
    })
}

// JSON 反序列化 + 序列化
async fn echo(user: web::Json<UserDTO>) -> impl Responder {
    HttpResponse::Ok().json(user.into_inner())
}

// 路径参数
async fn get_user(path: web::Path<String>) -> impl Responder {
    let id = path.into_inner();
    HttpResponse::Ok().json(UserResponse {
        name: format!("User {}", &id),
        user_id: id,
    })
}

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    HttpServer::new(|| {
        App::new()
            .route("/", web::get().to(hello))
            .route("/api/status", web::get().to(status))
            .route("/api/echo", web::post().to(echo))
            .route("/users/{id}", web::get().to(get_user))
    })
    .bind("0.0.0.0:8082")?
    .workers(4)
    .run()
    .await
}
