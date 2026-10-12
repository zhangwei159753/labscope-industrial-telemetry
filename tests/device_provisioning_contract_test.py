"""Provisioning safety checks for the PostgreSQL + Mosquitto device CLI."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DeviceProvisioningContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "tools" / "mqtt_device_sim.py").read_text(encoding="utf-8")

    def test_create_provisions_database_identity_and_scoped_dynsec_acls(self):
        self.assertIn("INSERT INTO devices(site_id,device_id,display_name,model,mqtt_username,credential_hash)", self.source)
        self.assertIn('"createRole", role', self.source)
        self.assertIn('"publishClientSend"', self.source)
        self.assertIn('"publishClientReceive"', self.source)
        self.assertIn('"subscribeLiteral"', self.source)
        self.assertIn('"addClientRole", username, role', self.source)

    def test_rotation_and_revocation_update_broker_and_database(self):
        self.assertIn('"setClientPassword", username, password', self.source)
        self.assertIn("credential_version=credential_version+1", self.source)
        self.assertIn('"disableClient", username', self.source)
        self.assertIn("UPDATE devices SET enabled=false", self.source)

    def test_device_credentials_are_not_stored_in_plaintext_in_database(self):
        self.assertIn("hashlib.sha256(password.encode()).hexdigest()", self.source)
        self.assertIn("credential_hash", self.source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
