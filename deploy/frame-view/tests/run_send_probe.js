"use strict";

const http = require("http");
const path = require("path");
const puppeteer = require("puppeteer-core");

const root = path.resolve(__dirname, "..");
const chrome = process.env.CHROME_BIN || "/usr/local/bin/google-chrome";

function serve() {
  return new Promise((resolve) => {
    const server = http.createServer((request, response) => {
      const url = new URL(request.url, "http://127.0.0.1");
      const file = path.normalize(path.join(root, decodeURIComponent(url.pathname)));
      if (!file.startsWith(root)) {
        response.writeHead(403);
        response.end();
        return;
      }
      const fs = require("fs");
      fs.readFile(file, (error, data) => {
        if (error) {
          response.writeHead(404);
          response.end();
          return;
        }
        const type = file.endsWith(".js") ? "text/javascript" : "text/html";
        response.writeHead(200, { "Content-Type": type, "Cache-Control": "no-store" });
        response.end(data);
      });
    });
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

(async () => {
  const server = await serve();
  const port = server.address().port;
  const browser = await puppeteer.launch({
    executablePath: chrome,
    headless: true,
    args: [
      "--no-sandbox",
      "--disable-gpu",
      "--use-fake-device-for-media-stream",
      "--use-fake-ui-for-media-stream",
      "--autoplay-policy=no-user-gesture-required"
    ]
  });
  try {
    const page = await browser.newPage();
    page.on("console", (message) => process.stdout.write(message.text() + "\n"));
    await page.goto(`http://127.0.0.1:${port}/tests/send_probe.html`, { waitUntil: "load" });
    await page.waitForFunction(() => window.__result, { timeout: 20000 });
    const result = await page.evaluate(() => window.__result);
    if (!result || !result.ok) {
      console.error("RESULT " + JSON.stringify(result));
      process.exit(1);
    }
    console.log("RESULT " + JSON.stringify(result));
  } finally {
    await browser.close();
    server.close();
  }
})().catch((error) => {
  console.error(String(error && error.stack || error));
  process.exit(1);
});
