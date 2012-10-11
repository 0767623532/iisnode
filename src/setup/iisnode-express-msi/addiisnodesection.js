// Installs iisnode configuration section into system.webServer section group in
// %userprofile%\documents\iisexpress\config\applicationHost.config (IIS Express)

function main() {
    var versionMgr = new ActiveXObject("Microsoft.IIS.VersionManager");
    var iisex = versionMgr.GetVersionObject("7.5", 2);
    var ahwrite = iisex.CreateObjectFromProgId("Microsoft.ApplicationHost.WritableAdminManager");
    var configManager = ahwrite.ConfigManager;
    var appHostConfig = configManager.GetConfigFile("MACHINE/WEBROOT/APPHOST");
    var systemWebServer = appHostConfig.RootSectionGroup.Item("system.webServer");
    try {
        systemWebServer.Sections.DeleteSection("iisnode");
    }
    catch (e) {
        // nothing to remove
    }
    var iisnode = systemWebServer.Sections.AddSection("iisnode");

    var webSocketSection;
    try {
        webSocketSection = systemWebServer.Sections.Item("webSocket");
    }
    catch (e) {
        // the section may not exist if we are on IIS 7
    }

    if (webSocketSection) {
        webSocketSection.overrideModeDefault = 'Allow';
    }

    ahwrite.CommitChanges();
}