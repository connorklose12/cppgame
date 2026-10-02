var app = WebApplication.Create(args);
app.UseDefaultFiles();
app.UseStaticFiles(new StaticFileOptions
{
    ServeUnknownFileTypes = true,   // Emscripten's index.data has no known MIME type
    // While developing, never let the browser reuse an old copy of index.js / index.wasm / index.data: always load the newest build.
    OnPrepareResponse = ctx => ctx.Context.Response.Headers["Cache-Control"] = "no-store"
});
app.Run("http://localhost:5000");