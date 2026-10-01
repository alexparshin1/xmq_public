import React from 'react';
import ControlAPI from '../ControlAPI';
import PageTitle from '../components/PageTitle';
import './InputScreen.css';
import './SessionsScreen.css';

export default class SessionsScreen extends React.Component {
    state = {
        prefix: '',
        limit: 100,
        sessions: [],
        hasMore: false,
        searched: false,
        loading: false,
        error: ''
    };

    componentDidMount() {
        this.search();
    }

    search = async (event) => {
        if (event) event.preventDefault();
        const {prefix, limit} = this.state;
        const count = Number(limit);
        if (!Number.isInteger(count) || count < 1 || count > 500) {
            this.setState({error: 'Limit must be between 1 and 500.'});
            return;
        }
        if (prefix.length > 256) {
            this.setState({error: 'Prefix must be 256 characters or fewer.'});
            return;
        }
        this.setState({loading: true, error: ''});
        const result = await ControlAPI.asyncMakeAPICall('GetClientSessions', {prefix, limit: count}, true);
        const error = result ? ControlAPI.apiError(result) : 'The XMQ interface did not answer the request.';
        this.setState({
            loading: false,
            searched: true,
            error: error || '',
            sessions: error ? [] : (result.client_sessions || []),
            hasMore: !error && result.has_more === true
        });
    };

    render() {
        const {prefix, limit, sessions, hasMore, searched, loading, error} = this.state;
        return <div className="InputScreen SessionsScreen">
            <PageTitle>Sessions</PageTitle>
            <form className="sessionsSearch" onSubmit={this.search}>
                <label>Client ID prefix
                    <input aria-label="Client ID prefix" value={prefix} maxLength={256}
                           onChange={event => this.setState({prefix: event.target.value})}
                           placeholder="All client IDs"/>
                </label>
                <label>Limit
                    <input aria-label="Limit" type="number" min="1" max="500" value={limit}
                           onChange={event => this.setState({limit: event.target.value})}/>
                </label>
                <button type="submit" disabled={loading}>{loading ? 'Searching…' : 'Search'}</button>
            </form>
            {error && <p role="alert" className="sessionsError">{error}</p>}
            {!error && searched && <p className="sessionsSummary">
                Showing {sessions.length} session{sessions.length === 1 ? '' : 's'}{hasMore ? '; more match this prefix. Enter more characters or raise the limit.' : '.'}
            </p>}
            <div className="sessionsTableWrap">
                <table className="sessionsTable">
                    <thead><tr>
                        <th>Client ID</th><th>Connected at</th><th>Status</th><th>Persistent</th>
                        <th title="Waiting or sent but not yet acknowledged">Queued messages</th><th>Subscriptions</th>
                    </tr></thead>
                    <tbody>
                    {sessions.map(session => <tr key={session.client_id}>
                        <td>{session.client_id}</td>
                        <td>{session.connected_at ? new Date(session.connected_at).toLocaleString() : '—'}</td>
                        <td>{session.online ? 'Online' : 'Offline'}</td>
                        <td>{session.persistent ? 'Yes' : 'No'}</td>
                        <td>{session.queued_messages}</td>
                        <td>{session.subscription_count}</td>
                    </tr>)}
                    </tbody>
                </table>
            </div>
        </div>;
    }
}
