var app = WebApplication.Create(args);
app.UseDefaultFiles();   // "/" opens index.html
app.UseStaticFiles();    // serves the game files in wwwroot
app.Run("http://localhost:5000");