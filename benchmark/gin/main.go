package main

import (
	"net/http"
	"runtime"

	"github.com/gin-gonic/gin"
)

type UserDTO struct {
	Name  string `json:"name"`
	Age   int    `json:"age"`
	Email string `json:"email"`
}

func main() {
	runtime.GOMAXPROCS(4)
	gin.SetMode(gin.ReleaseMode)
	r := gin.New()

	// Hello World
	r.GET("/", func(c *gin.Context) {
		c.String(http.StatusOK, "Hello, World!")
	})

	// JSON 响应
	r.GET("/api/status", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{
			"status":    "running",
			"framework": "gin",
		})
	})

	// JSON 反序列化 + 序列化
	r.POST("/api/echo", func(c *gin.Context) {
		var user UserDTO
		if err := c.ShouldBindJSON(&user); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}
		c.JSON(http.StatusOK, user)
	})

	// 路径参数
	r.GET("/users/:id", func(c *gin.Context) {
		id := c.Param("id")
		c.JSON(http.StatusOK, gin.H{
			"userId": id,
			"name":   "User " + id,
		})
	})

	r.Run(":8081")
}
