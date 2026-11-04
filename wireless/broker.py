"""MQTT authentication and exact-topic ACLs shared with the HTTP service."""
import hmac

from amqtt.contexts import Action
from amqtt.plugins.base import BaseAuthPlugin, BaseTopicPlugin

STORE = None
SERVICE_SECRET = None


class DeviceAuthPlugin(BaseAuthPlugin):
    async def authenticate(self, *, session):
        if session.username == "cloud":
            return session.client_id == "card-cloud" and hmac.compare_digest(
                session.password or "", SERVICE_SECRET or "")
        return bool(STORE and session.username == session.client_id and
                    STORE.device_auth(session.username, session.password or ""))


class DeviceAclPlugin(BaseTopicPlugin):
    async def topic_filtering(self, *, session=None, topic=None, action=None):
        if not session or not topic:
            return False
        if session.username == "cloud":
            return topic.startswith("cards/")
        device_id = session.username
        if not device_id or not STORE or not STORE.device(device_id):
            return False
        if action == Action.PUBLISH:
            return topic == f"cards/{device_id}/status"
        if action in (Action.SUBSCRIBE, Action.RECEIVE):
            return topic == f"cards/{device_id}/assignment"
        return False
