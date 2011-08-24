// Installs iisnode configuration section into system.webServer section group in 
// %systemroot%\system32\inetsrv\config\applicationHost.config

// This is inspired by http://www.ksingla.net/2007/02/using_ahadmin_to_read_write_iis_configuration_part_2/

try {
    var ahwrite = new ActiveXObject("Microsoft.ApplicationHost.WritableAdminManager");
    WScript.Echo('Obtained Microsoft.ApplicationHost.WritableAdminManager');
    var configManager = ahwrite.ConfigManager;
    WScript.Echo('Obtained ConfigManager');
    var appHostConfig = configManager.GetConfigFile("MACHINE/WEBROOT/APPHOST");
    WScript.Echo('Obtained applicationHost.config');
    var systemWebServer = appHostConfig.RootSectionGroup.Item("system.webServer");
    WScript.Echo('Found system.webServer section group');
    try {
        systemWebServer.Sections.DeleteSection("iisnode");
        WScript.Echo('Deleted iisnode section');
    }
    catch (e) {
        WScript.Echo('Did not find iisnode section to remove');
    }
    var iisnode = systemWebServer.Sections.AddSection("iisnode");
    WScript.Echo('Added iisnode section');
    ahwrite.CommitChanges();
    WScript.Echo('Committed changes');
    WScript.Quit(0);
} catch (e) {
    WScript.Quit(1);
}