import ConfigurationsScreen from "./ConfigurationsScreen";

export default class AuthenticationScreen extends ConfigurationsScreen
{
    page() { return "authentication"; }
    pageTitle() { return "Authentication"; }
    pageDescription() {
        return "Configured ways of authenticating clients. Their extensions must also be enabled " +
               "on the Installed Extensions page.";
    }
}
