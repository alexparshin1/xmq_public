import React from 'react';
import './App.css';
import './Primary.css'
import './screen/LoginScreen.jsx'
import {Route, Routes} from 'react-router-dom';
import HomeScreen from "./screen/Home";
import DashboardScreen from "./screen/DashboardScreen";
import RequireAuth from "./components/RequireAuth";
import ControlAPI from "./ControlAPI";
import MainMenu from "./components/MainMenu";
import LoginScreen from "./screen/LoginScreen";
import UsersScreen from "./screen/UsersScreen";
import UserGroupsScreen from "./screen/UserGroupsScreen";
import ListenersScreen from "./screen/Listeners";
import PersistenceScreen from "./screen/PersistenceScreen";
import SSLKeysScreen from "./screen/SSLKeysScreen";
import ServerLimitsScreen from "./screen/ServerLimitsScreen";
import LoggingScreen from "./screen/LoggingScreen";
import BridgesScreen from "./screen/BridgesScreen";
import XmqServiceScreen from "./screen/XmqServiceScreen";
import InitialSetupScreen from "./screen/InitialSetupScreen";
import UserManualScreen from "./screen/UserManualScreen";
import ExtensionsScreen from "./screen/ExtensionsScreen";

class App extends React.Component {

    state = {
        serverVersion: ControlAPI.serverVersion
    };

    componentDidMount() {
        this.unsubscribeVersion = ControlAPI.setOnServerVersionChange(
            version => this.setState({serverVersion: version}));
    }

    componentWillUnmount() {
        if (this.unsubscribeVersion) {
            this.unsubscribeVersion();
        }
    }

    render() {
        return (
            <div className="App">
                <table style={{height: "800px", borderSpacing: 0}} className="ContentTable">
                    <tbody>
                    <tr style={{height: "30px"}} className="XMQ-panel">
                        <td style={{width: '200px'}}></td>
                        <td style={{width: '90%', textAlign: 'left'}}>
                            {/* The version is the server's own, shown once it has reported it.
                                Nothing is shown before then rather than a guess: an interface
                                naming a version it has not been told is wrong as often as not. */}
                            XMQ server{this.state.serverVersion ? " " + this.state.serverVersion : ""}
                            {" "}(C)opyright 2024-2026 Alexey Parshin.
                        </td>
                    </tr>
                    <tr>
                        <td className="XMQ-panel" style={{textAlign: 'left', verticalAlign: 'top'}}>
                            <MainMenu/>
                        </td>
                        <td style={{verticalAlign: 'top', padding: '10px'}}>
                            <div className='content'>
                                <Routes>
                                    {/* Open to everyone: signing in, signing out, and the manual -
                                        installing and tuning the host happen before there is
                                        anything to sign in to. */}
                                    <Route path="/" Component={HomeScreen}/>
                                    <Route path="/login" Component={LoginScreen}/>
                                    <Route path="/manual" Component={UserManualScreen}/>
                                    <Route path="/extensions" Component={ExtensionsScreen}/>

                                    {/* Everything else needs a session. Wrapped rather than
                                        checked inside each screen, so a page cannot stay on
                                        display after the session behind it has expired. */}
                                    <Route path="/dashboard" element={<RequireAuth><DashboardScreen/></RequireAuth>}/>
                                    <Route path="/users" element={<RequireAuth><UsersScreen/></RequireAuth>}/>
                                    <Route path="/usergroups" element={<RequireAuth><UserGroupsScreen/></RequireAuth>}/>
                                    <Route path="/listeners" element={<RequireAuth><ListenersScreen/></RequireAuth>}/>
                                    <Route path="/persistence" element={<RequireAuth><PersistenceScreen/></RequireAuth>}/>
                                    <Route path="/sslkeys" element={<RequireAuth><SSLKeysScreen/></RequireAuth>}/>
                                    <Route path="/serverlimits" element={<RequireAuth><ServerLimitsScreen/></RequireAuth>}/>
                                    <Route path="/logging" element={<RequireAuth><LoggingScreen/></RequireAuth>}/>
                                    <Route path="/service" element={<RequireAuth><XmqServiceScreen/></RequireAuth>}/>
                                    <Route path="/bridges" element={<RequireAuth><BridgesScreen/></RequireAuth>}/>
                                    <Route path="/setup" element={<RequireAuth><InitialSetupScreen/></RequireAuth>}/>
                                </Routes>
                            </div>
                        </td>
                    </tr>
                    </tbody>
                </table>
            </div>
        );
    }
}

export default App;
