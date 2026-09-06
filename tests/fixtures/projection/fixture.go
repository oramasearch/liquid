package main

import (
	"fmt"
	"os"
	"time"
)

func main() {
	if len(os.Args) == 1 {
		fmt.Println("liquid projection fixture")
		return
	}
	for {
		time.Sleep(time.Hour)
	}
}
