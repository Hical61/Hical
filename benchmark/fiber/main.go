package main

import (
	"fmt"
	"runtime"

	"github.com/gofiber/fiber/v2"
)

type UserDTO struct {
	Name  string `json:"name"`
	Age   int    `json:"age"`
	Email string `json:"email"`
}

func main() {
	runtime.GOMAXPROCS(4)

	app := fiber.New(fiber.Config{
		DisableStartupMessage: true,
		Prefork:               false,
	})

	// Hello World
	app.Get("/", func(c *fiber.Ctx) error {
		return c.SendString("Hello, World!")
	})

	// JSON 响应
	app.Get("/api/status", func(c *fiber.Ctx) error {
		return c.JSON(fiber.Map{
			"status":    "running",
			"framework": "fiber",
		})
	})

	// JSON 反序列化 + 序列化
	app.Post("/api/echo", func(c *fiber.Ctx) error {
		var user UserDTO
		if err := c.BodyParser(&user); err != nil {
			return c.Status(400).JSON(fiber.Map{"error": err.Error()})
		}
		return c.JSON(user)
	})

	// 路径参数
	app.Get("/users/:id", func(c *fiber.Ctx) error {
		id := c.Params("id")
		return c.JSON(fiber.Map{
			"userId": id,
			"name":   fmt.Sprintf("User %s", id),
		})
	})

	app.Listen(":8089")
}
