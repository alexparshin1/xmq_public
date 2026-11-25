import ConfigurationsScreen from "./ConfigurationsScreen";

export default class DataIntegrationsScreen extends ConfigurationsScreen
{
    page() { return "data-integrations"; }
    pageTitle() { return "Data Integrations"; }
    pageDescription() {
        return "Configured exports and imports. An integration runs only when its extension is " +
               "enabled here and enabled on the Installed Extensions page.";
    }
}
