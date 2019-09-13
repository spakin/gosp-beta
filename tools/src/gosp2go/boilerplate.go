// This file defines boilerplate code that forms part of every
// generated Go file.

package main

// header is included at the top of every generated Go file.
var header = `// This file was generated by gosp2go.

package main

import (
	gospBytes "bytes"
	gospJson "encoding/json"
	gospFlag "flag"
	gospFmt "fmt"
	gospIo "io"
	gospNet "net"
	gospHttp "net/http"
	gospOs "os"
	gospFilePath "path/filepath"
	gospSync "sync"
	gospAtomic "sync/atomic"
	gospTime "time"
)

`

// body begins the function that was converted from a Gosp page to Go.
var bodyBegin = `// GospGenerateHTML represents the user's Gosp page, converted to Go.
func GospGenerateHTML(gospReq *GospRequest, gospOut gospIo.Writer, gospMeta chan<- GospKeyValue) {
	// On exit, close the metadata channel.  If the user's code panicked,
	// change the return code to "internal server error".
	defer func() {
		if r := recover(); r != nil {
			gospMeta <- GospKeyValue{
				Key:   "http-status",
				Value: gospFmt.Sprint(gospHttp.StatusInternalServerError),
			}
		}
		close(gospMeta)
	}()

	// Provide functions for passing metadata back to the Web server.
	GospSetHttpStatus := func(s int) {
		gospMeta <- GospKeyValue{Key: "http-status", Value: gospFmt.Sprint(s)}
	}
	GospSetMimeType := func(mt string) {
		gospMeta <- GospKeyValue{Key: "mime-type", Value: mt}
	}
	GospSetHeaderField := func(k, v string, repl bool) {
		gospMeta <- GospKeyValue{
			Key:   "header-field",
			Value: gospFmt.Sprintf("%v %s %s", repl, k, v),
		}
	}
	GospHeartbeat := func() {
		gospMeta <- GospKeyValue{Key: "keep-alive", Value: ""}
	}
	_, _, _, _ = GospSetHttpStatus, GospSetMimeType, GospSetHeaderField, GospHeartbeat

	// Change to the directory containing the Go Server Page.
	if gospReq.Filename != "" {
		err := gospOs.Chdir(gospFilePath.Dir(gospReq.Filename))
		if err != nil {
			panic(err) // Returns a StatusInternalServerError
		}
	}

	// Express the Gosp page in Go.
`

// trailer is included at the end of every generated Go file.
var trailer = `
// GospLaunchHTMLGenerator starts GospGenerateHTML in a separate goroutine and
// waits for it to finish.
func GospLaunchHTMLGenerator(gospOut gospIo.Writer, gospReq *GospRequest) {
	// Spawn GospGenerateHTML, giving it a buffer in which to write HTML
	// and a channel in which to send metadata.
	html := gospBytes.NewBuffer(nil)
	meta := make(chan GospKeyValue, 5)
	go GospGenerateHTML(gospReq, html, meta)

	// Read metadata from GospGenerateHTML until no more remains.
	okStr := gospFmt.Sprint(gospHttp.StatusOK)
	status := okStr
	for kv := range meta {
		switch kv.Key {
		case "mime-type", "http-status", "header-field", "keep-alive", "debug-message":
			gospFmt.Fprintf(gospOut, "%s %s\n", kv.Key, kv.Value)
		}

		// Keep track of the current HTTP status code.
		if kv.Key == "http-status" {
			status = kv.Value
		}
	}
	gospFmt.Fprintln(gospOut, "end-header")
	if status == okStr {
		gospFmt.Fprint(gospOut, html)
	}
}

// A GospRequest encapsulates Web-server information passed into this program.
// This data structure must be kept up-to-date with the send_request() function
// in comm.c
type GospRequest struct {
	Scheme         string            // HTTP scheme ("http" or "https")
	LocalHostname  string            // Name of the local host
	Port           int               // Port number to which the request was issued
	Uri            string            // Path portion of the URI
	PathInfo       string            // Additional text following the Gosp filename
	QueryArgs      string            // Query arguments from the request
	Url            string            // Complete URL requested
	Method         string            // Request method ("GET", "POST", etc.)
	RequestLine    string            // First line of the request (e.g., "GET / HTTP/1.1")
	RequestTime    int64             // Request time in nanoseconds since the Unix epoch
	RemoteHostname string            // Name of the remote host
	RemoteIp       string            // IP address of the remote host
	Filename       string            // Local filename of the Gosp page
	PostData       map[string]string // {Key, value} pairs sent by a POST request
	HeaderData     map[string]string // Request headers as {key, value} pairs
	AdminEmail     string            // Email address of the Web server administrator
	Environment    map[string]string // Environment variables passed in from the server
	ExitNow        bool              // Used internally: If true, shut down the program cleanly
}

// A GospKeyValue represents a metadata key:value pair.
type GospKeyValue struct {
	Key   string
	Value string
}

// GospRequestFromFile reads a GospRequest from a named JSON file and passes
// this to GospLaunchHTMLGenerator.
func GospRequestFromFile(fn string) error {
	f, err := gospOs.Open(fn)
	if err != nil {
		return err
	}
	defer f.Close()
	dec := gospJson.NewDecoder(f)
	var gr GospRequest
	err = dec.Decode(&gr)
	if err != nil {
		return err
	}
	GospLaunchHTMLGenerator(gospOs.Stdout, nil)
	return nil
}

// GospResetKillClock adds time to the auto-kill clock.
func GospResetKillClock(t *gospTime.Timer, d gospTime.Duration) {
	if d == 0 {
		return
	}
	if !t.Stop() {
		<-t.C
	}
	t.Reset(d)
}

// GospStartServer runs the program in server mode.  It accepts a connection on
// a Unix-domain socket, reads a GospRequest in JSON format, and spawns
// GospLaunchHTMLGenerator to respond to the request.  The server terminates
// when given a request with ExitNow set to true.
func GospStartServer(fn string) error {
	// Server code should write only to the io.Writer it's given and not
	// read at all.
	gospOs.Stdin.Close()
	gospOs.Stdout.Close()

	// Listen on the named Unix-domain socket.
	sock, err := gospFilePath.Abs(fn)
	if err != nil {
		return err
	}
	_ = gospOs.Remove(sock) // It's not an error if the socket doesn't exist.
	ln, err := gospNet.Listen("unix", sock)
	if err != nil {
		return err
	}

	// Exit automatically after gospAutoKill minutes of no activity.
	var killClk *gospTime.Timer
	if gospAutoKillTime > 0 {
		killClk = gospTime.AfterFunc(gospAutoKillTime*gospTime.Minute, func() {
			_ = gospOs.Remove(sock)
			gospOs.Exit(0)
		})
	}

	// Process connections until we're told to stop.
	var done int32
	var wg gospSync.WaitGroup
	for gospAtomic.LoadInt32(&done) == 0 {
		// Spawn a goroutine to handle the incoming connection.
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		GospResetKillClock(killClk, gospAutoKillTime*gospTime.Minute)
		wg.Add(1)
		go func(conn gospNet.Conn) {
			// Parse the request as a JSON object.
			defer wg.Done()
			defer conn.Close()
			conn.SetDeadline(gospTime.Now().Add(10 * gospTime.Second))
			dec := gospJson.NewDecoder(conn)
			var gr GospRequest
			err = dec.Decode(&gr)
			if err != nil {
				return
			}

			// If we were asked to exit, send back our PID, notify
			// our parent, and establish a dummy connection to
			// flush the pipeline.
			if gr.ExitNow {
				gospFmt.Fprintf(conn, "gosp-pid %d\n", gospOs.Getpid())
				gospAtomic.StoreInt32(&done, 1)
				c, err := gospNet.Dial("unix", sock)
				if err == nil {
					c.Close()
				}
				return
			}

			// Pass the request to the user-defined Gosp code.
			GospLaunchHTMLGenerator(conn, &gr)
		}(conn)
	}

	// Wait until all existing requests complete before we return.
	wg.Wait()
	return nil
}

func main() {
	var err error
	gospOs.Stdin = nil
	sock := gospFlag.String("socket", "", "Unix socket (filename) on which to listen for JSON code")
	file := gospFlag.String("file", "", "File name from which to read JSON code")
	gospFlag.Parse()
	switch {
	case *file != "":
		err = GospRequestFromFile(*file)
	case *sock != "":
		err = GospStartServer(*sock)
	default:
		GospLaunchHTMLGenerator(gospOs.Stdout, nil)
	}
	if err != nil {
		panic(err)
	}
	return
}
`