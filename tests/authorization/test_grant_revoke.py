# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Client tests for SQL statement authorization

import grp
import pytest
from getpass import getuser
from os import getenv
from time import sleep
from  __builtin__ import any as b_any

from tests.common.custom_cluster_test_suite import CustomClusterTestSuite
from tests.common.impala_test_suite import ImpalaTestSuite
from tests.common.test_dimensions import create_uncompressed_text_dimension
from tests.verifiers.metric_verifier import MetricVerifier

SENTRY_CONFIG_FILE = getenv('IMPALA_HOME') + '/fe/src/test/resources/sentry-site.xml'

class TestGrantRevoke(CustomClusterTestSuite, ImpalaTestSuite):
  @classmethod
  def add_test_dimensions(cls):
    super(TestGrantRevoke, cls).add_test_dimensions()
    cls.ImpalaTestMatrix.add_dimension(
        create_uncompressed_text_dimension(cls.get_workload()))

  @classmethod
  def get_workload(cls):
    return 'functional-query'

  def setup_method(self, method):
    super(TestGrantRevoke, self).setup_method(method)
    self.__test_cleanup()

  def teardown_method(self, method):
    self.__test_cleanup()
    super(TestGrantRevoke, self).teardown_method(method)

  def __test_cleanup(self):
    # Clean up any old roles created by this test
    for role_name in self.client.execute("show roles").data:
      if 'grant_revoke_test' in role_name:
        self.client.execute("drop role %s" % role_name)

    # Cleanup any other roles that were granted to this user.
    # TODO: Update Sentry Service config and authorization tests to use LocalGroupMapping
    # for resolving users -> groups. This way we can specify custom test users that don't
    # actually exist in the system.
    group_name = grp.getgrnam(getuser()).gr_name
    for role_name in self.client.execute("show role grant group `%s`" % group_name).data:
      self.client.execute("drop role %s" % role_name)

    # Create a temporary admin user so we can actually view/clean up the test
    # db.
    self.client.execute("create role grant_revoke_test_admin")
    try:
      self.client.execute("grant all on server to grant_revoke_test_admin")
      self.client.execute("grant role grant_revoke_test_admin to group %s" % group_name)
      self.cleanup_db('grant_rev_db', sync_ddl=0)
    finally:
      self.client.execute("drop role grant_revoke_test_admin")

  @classmethod
  def restart_first_impalad(cls):
    impalad = cls.cluster.impalads[0]
    impalad.restart()
    cls.client = impalad.service.create_beeswax_client()

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args(
      impalad_args="--server_name=server1",
      catalogd_args="--sentry_config=" + SENTRY_CONFIG_FILE)
  def test_grant_revoke(self, vector):
    self.run_test_case('QueryTest/grant_revoke', vector, use_db="default")


  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args(
      impalad_args="--server_name=server1",
      catalogd_args="--sentry_config=" + SENTRY_CONFIG_FILE,
      statestored_args=("--statestore_heartbeat_frequency_ms=300 "
                        "--statestore_update_frequency_ms=300"))
  def test_role_update(self, vector):
    """IMPALA-5355: The initial update from the statestore has the privileges and roles in
    reverse order if a role was modified, but not the associated privilege. Verify that
    Impala is able to handle this.
    """
    self.client.execute("create role test_role")
    self.client.execute("grant all on server to test_role")
    # Wait a few seconds to make sure the update propagates to the statestore.
    sleep(3)
    # Update the role, increasing its catalog verion.
    self.client.execute("grant role test_role to group {0}".format(
        grp.getgrnam(getuser()).gr_name))
    result = self.client.execute("show tables in functional")
    assert 'alltypes' in result.data
    privileges_before = self.client.execute("show grant role test_role")
    # Wait a few seconds before restarting Impalad to make sure that the Catalog gets
    # updated.
    sleep(3)
    self.restart_first_impalad()
    verifier = MetricVerifier(self.cluster.impalads[0].service)
    verifier.wait_for_metric("catalog.ready", True)
    # Verify that we still have the right privileges after the first impalad was
    # restarted.
    result = self.client.execute("show tables in functional");
    assert 'alltypes' in result.data
    privileges_after = self.client.execute("show grant role test_role")
    assert privileges_before.data == privileges_after.data

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args(
      impalad_args="--server_name=server1",
      catalogd_args="--sentry_config=" + SENTRY_CONFIG_FILE +
                    " --sentry_catalog_polling_frequency=10",
      statestored_args=("--statestore_heartbeat_frequency_ms=300 "
                        "--statestore_update_frequency_ms=300"))
  def test_role_privilege_case(self, vector):
    """IMPALA-5582: Store sentry privileges in lower case. This
    test grants select privileges to tables specified in lower,
    upper and mix cases. This test verifies that these privileges
    do not vanish on a sentryProxy thread update.
    """
    self.client.execute("create role test_role")
    self.client.execute("grant all on server to test_role")
    self.client.execute("grant role test_role to group {0}".format(
        grp.getgrnam(getuser()).gr_name))

    self.client.execute("create database grant_rev_db")
    self.client.execute("use grant_rev_db")
    self.client.execute("create table if not exists test1(i int)")
    self.client.execute("create table if not exists TEST2(i int)")
    self.client.execute("create table if not exists Test3(i int)")

    self.client.execute("grant select on table test1 to test_role")
    self.client.execute("grant select on table TEST2 to test_role")
    self.client.execute("grant select on table TesT3 to test_role")
    result = self.client.execute("show grant role test_role")
    assert b_any('test1' in x for x in result.data)
    assert b_any('test2' in x for x in result.data)
    assert b_any('test3' in x for x in result.data)
    # Sleep for 15 seconds and make sure that the privileges
    # on all 3 tables still persist on a sentryProxy thread
    # update. sentry_catalog_polling_frequency is set to 10
    # seconds.
    sleep(15)
    result = self.client.execute("show grant role test_role")
    assert b_any('test1' in x for x in result.data)
    assert b_any('test2' in x for x in result.data)
    assert b_any('test3' in x for x in result.data)
    self.client.execute("drop role test_role")
