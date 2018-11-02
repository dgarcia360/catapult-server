/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "nodediscovery/src/NodeDiscoveryService.h"
#include "catapult/ionet/NetworkNode.h"
#include "catapult/ionet/PacketSocket.h"
#include "catapult/net/ServerConnector.h"
#include "catapult/net/VerifyPeer.h"
#include "nodediscovery/tests/test/NodeDiscoveryTestUtils.h"
#include "tests/test/core/PacketPayloadTestUtils.h"
#include "tests/test/core/ThreadPoolTestUtils.h"
#include "tests/test/core/mocks/MockPacketIo.h"
#include "tests/test/local/ServiceLocatorTestContext.h"
#include "tests/test/local/ServiceTestUtils.h"
#include "tests/test/net/BriefServerRequestorTestUtils.h"
#include "tests/test/net/NodeTestUtils.h"
#include "tests/test/net/SocketTestUtils.h"
#include "tests/test/net/mocks/MockPacketWriters.h"
#include "tests/TestHarness.h"

namespace catapult { namespace nodediscovery {

#define TEST_CLASS NodeDiscoveryServiceTests

	namespace {
		constexpr auto Service_Name = "nd.ping_requestor";
		constexpr auto Active_Counter_Name = "ACTIVE PINGS";
		constexpr auto Total_Counter_Name = "TOTAL PINGS";
		constexpr auto Success_Counter_Name = "SUCCESS PINGS";
		constexpr auto Sentinel_Counter_Value = extensions::ServiceLocator::Sentinel_Counter_Value;

		constexpr auto Num_Expected_Tasks = 2;
		constexpr auto Ping_Task_Name = "node discovery ping task";
		constexpr auto Peers_Task_Name = "node discovery peers task";

		auto CreateLocalNetworkNode() {
			auto pNetworkNode = std::make_shared<ionet::NetworkNode>();
			test::FillWithRandomData({ reinterpret_cast<uint8_t*>(pNetworkNode.get()), sizeof(ionet::NetworkNode) });
			pNetworkNode->Size = sizeof(ionet::NetworkNode);
			pNetworkNode->HostSize = 0;
			pNetworkNode->FriendlyNameSize = 0;
			return pNetworkNode;
		}

		struct NodeDiscoveryServiceTraits {
			static auto CreateRegistrar(const std::shared_ptr<const ionet::NetworkNode>& pLocalNetworkNode) {
				return CreateNodeDiscoveryServiceRegistrar(pLocalNetworkNode);
			}

			static auto CreateRegistrar() {
				return CreateRegistrar(CreateLocalNetworkNode());
			}
		};

		class TestContext : public test::ServiceLocatorTestContext<NodeDiscoveryServiceTraits> {
		public:
			TestContext() {
				// register dependent hooks
				testState().state().hooks().addPacketPayloadSink([&payloads = m_payloads](const auto& payload) {
					payloads.push_back(payload);
				});
			}

		public:
			const auto& payloads() const {
				return m_payloads;
			}

		private:
			std::vector<ionet::PacketPayload> m_payloads;
		};

		void AssertNoPushNodeConsumerCalls(TestContext& context) {
			// Assert: subscriber wasn't called
			const auto& subscriber = context.testState().nodeSubscriber();
			EXPECT_EQ(0u, subscriber.nodeParams().params().size());

			// - nodes weren't modified
			auto& nodes = context.testState().state().nodes();
			EXPECT_EQ(0u, nodes.view().size());
		}
	}

	// region basic

	ADD_SERVICE_REGISTRAR_INFO_TEST(NodeDiscovery, Post_Packet_Io_Pickers)

	TEST(TEST_CLASS, CanBootService) {
		// Arrange:
		TestContext context;

		// Act:
		context.boot();

		// Assert:
		EXPECT_EQ(1u, context.locator().numServices());
		EXPECT_EQ(3u, context.locator().counters().size());

		EXPECT_TRUE(!!context.locator().service<void>(Service_Name));

		EXPECT_EQ(0u, context.counter(Active_Counter_Name));
		EXPECT_EQ(0u, context.counter(Total_Counter_Name));
		EXPECT_EQ(0u, context.counter(Success_Counter_Name));
	}

	TEST(TEST_CLASS, CanShutdownService) {
		// Arrange:
		TestContext context;

		// Act:
		context.boot();
		context.shutdown();

		// Assert:
		EXPECT_EQ(1u, context.locator().numServices());
		EXPECT_EQ(3u, context.locator().counters().size());

		EXPECT_FALSE(!!context.locator().service<void>(Service_Name));

		EXPECT_EQ(Sentinel_Counter_Value, context.counter(Active_Counter_Name));
		EXPECT_EQ(Sentinel_Counter_Value, context.counter(Total_Counter_Name));
		EXPECT_EQ(Sentinel_Counter_Value, context.counter(Success_Counter_Name));
	}

	TEST(TEST_CLASS, PacketHandlersAreRegistered) {
		// Arrange:
		TestContext context;

		// Act:
		context.boot();
		const auto& handlers = context.testState().state().packetHandlers();

		// Assert:
		EXPECT_EQ(4u, handlers.size());
		EXPECT_TRUE(handlers.canProcess(ionet::PacketType::Node_Discovery_Push_Ping));
		EXPECT_TRUE(handlers.canProcess(ionet::PacketType::Node_Discovery_Pull_Ping));
		EXPECT_TRUE(handlers.canProcess(ionet::PacketType::Node_Discovery_Push_Peers));
		EXPECT_TRUE(handlers.canProcess(ionet::PacketType::Node_Discovery_Pull_Peers));
	}

	// endregion

	// region handlers

	namespace {
		auto CreateNodePushPingPacket(const Key& identityKey, const std::string& host, const std::string& name) {
			return test::CreateNodePushPingPacket(identityKey, ionet::NodeVersion(1234), host, name);
		}

		auto CreateNodePullPingPacket(const Key& identityKey, const std::string& host, const std::string& name) {
			auto pPacket = test::CreateNodePushPingPacket(identityKey, ionet::NodeVersion(1234), host, name);
			pPacket->Type = ionet::PacketType::Node_Discovery_Pull_Ping;
			return pPacket;
		}

		class PullPingServer : public test::RemotePullServer {
		public:
			void prepareValidResponse(const crypto::KeyPair& partnerKeyPair, const std::string& name) {
				auto pResponsePacket = CreateNodePullPingPacket(partnerKeyPair.publicKey(), "127.0.0.1", name);
				test::RemotePullServer::prepareValidResponse(partnerKeyPair, pResponsePacket);
			}
		};
	}

	TEST(TEST_CLASS, PushPingNotifiesSubscribersAboutNewNodes) {
		// Arrange:
		TestContext context;
		context.boot();

		auto identityKey = test::GenerateRandomData<Key_Size>();
		auto pPacket = CreateNodePushPingPacket(identityKey, "alice.com", "the GREAT");

		// Act:
		ionet::ServerPacketHandlerContext handlerContext(identityKey, std::string());
		const auto& handlers = context.testState().state().packetHandlers();
		handlers.process(*pPacket, handlerContext);

		// Assert:
		const auto& subscriber = context.testState().nodeSubscriber();
		ASSERT_EQ(1u, subscriber.nodeParams().params().size());

		const auto& subscriberNode = subscriber.nodeParams().params()[0].NodeCopy;
		EXPECT_EQ(identityKey, subscriberNode.identityKey());
		EXPECT_EQ("alice.com", subscriberNode.endpoint().Host);
		EXPECT_EQ(ionet::NodeVersion(1234), subscriberNode.metadata().Version);

		// - check counters (should not be affected)
		EXPECT_EQ(0u, context.counter(Total_Counter_Name));
		EXPECT_EQ(0u, context.counter(Success_Counter_Name));
	}

	TEST(TEST_CLASS, PushPeersNotifiesSubscribersAboutNewNodes) {
		// Arrange:
		TestContext context;
		context.boot();

		// - simulate the remote node by responding with compatible (but slightly different) node information
		auto partnerKeyPair = test::GenerateKeyPair();
		PullPingServer pullPingServer;
		pullPingServer.prepareValidResponse(partnerKeyPair, "the Legend");

		// - prepare a packet that simulates peers pushed from another node (and uses the local node as the forwarded peer enpoint)
		auto pRequestPacket = CreateNodePullPingPacket(partnerKeyPair.publicKey(), "127.0.0.1", "the GREAT");
		pRequestPacket->Type = ionet::PacketType::Node_Discovery_Push_Peers;
		reinterpret_cast<ionet::NetworkNode&>(*pRequestPacket->Data()).Port = test::Local_Host_Port;

		// Act:
		ionet::ServerPacketHandlerContext handlerContext(test::GenerateRandomData<Key_Size>(), std::string());
		const auto& handlers = context.testState().state().packetHandlers();
		handlers.process(*pRequestPacket, handlerContext);

		// - wait for success
		WAIT_FOR_ONE_EXPR(context.counter(Success_Counter_Name));
		WAIT_FOR_ZERO_EXPR(context.counter(Active_Counter_Name));

		// Assert: subscriber was called with response node (the name is the differentiator)
		const auto& subscriber = context.testState().nodeSubscriber();
		ASSERT_EQ(1u, subscriber.nodeParams().params().size());

		const auto& subscriberNode = subscriber.nodeParams().params()[0].NodeCopy;
		EXPECT_EQ(partnerKeyPair.publicKey(), subscriberNode.identityKey());
		EXPECT_EQ("127.0.0.1", subscriberNode.endpoint().Host);
		EXPECT_EQ("the Legend", subscriberNode.metadata().Name);

		// - success counter was incremented
		EXPECT_EQ(1u, context.counter(Total_Counter_Name));
		EXPECT_EQ(1u, context.counter(Success_Counter_Name));
	}

	TEST(TEST_CLASS, PushPeersDoesNotNotifySubscribersAboutPreviouslyKnownNodes) {
		// Arrange:
		TestContext context;
		context.boot();

		// - add the remote node to the node container
		auto partnerKeyPair = test::GenerateKeyPair();
		auto& nodes = context.testState().state().nodes();
		nodes.modifier().add(test::CreateNamedNode(partnerKeyPair.publicKey(), "bob"), ionet::NodeSource::Static);

		// - prepare a packet that simulates peers pushed from another node (and uses the local node as the forwarded peer enpoint)
		auto pRequestPacket = CreateNodePullPingPacket(partnerKeyPair.publicKey(), "127.0.0.1", "the GREAT");
		pRequestPacket->Type = ionet::PacketType::Node_Discovery_Push_Peers;
		reinterpret_cast<ionet::NetworkNode&>(*pRequestPacket->Data()).Port = test::Local_Host_Port;

		// Act:
		ionet::ServerPacketHandlerContext handlerContext(test::GenerateRandomData<Key_Size>(), std::string());
		const auto& handlers = context.testState().state().packetHandlers();
		handlers.process(*pRequestPacket, handlerContext);

		// - wait for some processing to happen (there is nothing to wait on because nothing should change)
		test::Pause();

		// Assert: subscriber wasn't called
		const auto& subscriber = context.testState().nodeSubscriber();
		EXPECT_EQ(0u, subscriber.nodeParams().params().size());

		// - nodes weren't modified
		EXPECT_EQ(1u, nodes.view().size());

		// - no counters were changed
		EXPECT_EQ(0u, context.counter(Total_Counter_Name));
		EXPECT_EQ(0u, context.counter(Success_Counter_Name));
	}

	TEST(TEST_CLASS, PushPeersDoesNotNotifySubscribersAboutFailedNodes) {
		// Arrange:
		TestContext context;
		context.boot();

		// - simulate the bad remote node by not responding to the challenge
		auto partnerKeyPair = test::GenerateKeyPair();
		PullPingServer pullPingServer;
		pullPingServer.prepareNoResponse();

		// - prepare a packet that simulates peers pushed from another node (and uses the local node as the forwarded peer enpoint)
		auto pRequestPacket = CreateNodePullPingPacket(partnerKeyPair.publicKey(), "127.0.0.1", "the GREAT");
		pRequestPacket->Type = ionet::PacketType::Node_Discovery_Push_Peers;
		reinterpret_cast<ionet::NetworkNode&>(*pRequestPacket->Data()).Port = test::Local_Host_Port;

		// Act:
		ionet::ServerPacketHandlerContext handlerContext(test::GenerateRandomData<Key_Size>(), std::string());
		const auto& handlers = context.testState().state().packetHandlers();
		handlers.process(*pRequestPacket, handlerContext);

		// - wait for an active connection
		WAIT_FOR_EXPR(pullPingServer.hasConnection());
		WAIT_FOR_ONE_EXPR(context.counter(Active_Counter_Name));

		// Sanity: a connection is active
		EXPECT_EQ(1u, context.counter(Active_Counter_Name));

		// Act: close the connection
		pullPingServer.close();
		WAIT_FOR_ZERO_EXPR(context.counter(Active_Counter_Name));

		// Assert: push node consumer wasn't called
		AssertNoPushNodeConsumerCalls(context);

		// - attempt counter was incremented
		EXPECT_EQ(1u, context.counter(Total_Counter_Name));
		EXPECT_EQ(0u, context.counter(Success_Counter_Name));
	}

	// endregion

	// region ping task

	TEST(TEST_CLASS, PingTaskIsScheduled) {
		// Assert:
		test::AssertRegisteredTask(TestContext(), Num_Expected_Tasks, Ping_Task_Name);
	}

	TEST(TEST_CLASS, PingTaskBroadcastsLocalNetworkNode) {
		// Arrange:
		TestContext context;

		auto pLocalNetworkNode = CreateLocalNetworkNode();
		context.boot(pLocalNetworkNode);

		const auto& payloads = context.payloads();
		test::RunTaskTestPostBoot(context, Num_Expected_Tasks, Ping_Task_Name, [&payloads, &node = *pLocalNetworkNode](const auto& task) {
			// Act:
			auto result = task.Callback().get();

			// Assert:
			EXPECT_EQ(thread::TaskResult::Continue, result);

			// - a single packet was broadcasted
			ASSERT_EQ(1u, payloads.size());

			auto expectedPacketSize = sizeof(ionet::PacketHeader) + sizeof(ionet::NetworkNode);
			const auto& payload = payloads[0];
			test::AssertPacketHeader(payload, expectedPacketSize, ionet::PacketType::Node_Discovery_Push_Ping);
			ASSERT_EQ(1u, payload.buffers().size());

			const auto& buffer = payload.buffers()[0];
			ASSERT_EQ(sizeof(ionet::NetworkNode), buffer.Size);
			EXPECT_TRUE(0 == memcmp(&node, buffer.pData, buffer.Size));
		});
	}

	// endregion

	// region peers task

	TEST(TEST_CLASS, PeersTaskIsScheduled) {
		// Assert:
		test::AssertRegisteredTask(TestContext(), Num_Expected_Tasks, Peers_Task_Name);
	}

	TEST(TEST_CLASS, PeersTaskHasNoEffectWhenNoPacketIosAreAvailable) {
		// Arrange:
		TestContext context;
		test::RunTaskTest(context, Num_Expected_Tasks, Peers_Task_Name, [&context](const auto& task) {
			// Act:
			auto result = task.Callback().get();

			// Assert:
			EXPECT_EQ(thread::TaskResult::Continue, result);

			// Assert: push node consumer wasn't called
			AssertNoPushNodeConsumerCalls(context);
		});
	}

	TEST(TEST_CLASS, PeersTaskForwardsToConsumerWhenApiSucceeds) {
		// Arrange: create a single picker that returns a packet io with a successful interaction
		auto partnerKeyPair = test::GenerateKeyPair();
		const auto& partnerKey = partnerKeyPair.publicKey();
		auto pPacketIo = std::make_shared<mocks::MockPacketIo>();
		pPacketIo->queueWrite(ionet::SocketOperationCode::Success);
		pPacketIo->queueRead(ionet::SocketOperationCode::Success, [&partnerKey](const auto*) {
			// - push ping and pull peers packets are compatible, so create the former and change the type
			auto pPacket = CreateNodePushPingPacket(partnerKey, "127.0.0.1", "alice");
			reinterpret_cast<ionet::NetworkNode&>(*pPacket->Data()).Port = test::Local_Host_Port;
			pPacket->Type = ionet::PacketType::Node_Discovery_Pull_Peers;
			return pPacket;
		});

		// - simulate the remote node by responding with compatible (but slightly different) node information
		PullPingServer pullPingServer;
		pullPingServer.prepareValidResponse(partnerKeyPair, "the Legend");

		// - ensure the packet lifetime is extended by the task callback by
		//  (1) simulating its removal from writers after being returned by pickOne
		//  (2) delaying all io operations
		mocks::PickOneAwareMockPacketWriters picker(mocks::PickOneAwareMockPacketWriters::SetPacketIoBehavior::Use_Once);
		pPacketIo->setDelay(utils::TimeSpan::FromMilliseconds(50));
		picker.setPacketIo(std::move(pPacketIo));

		TestContext context;
		context.testState().state().packetIoPickers().insert(picker, ionet::NodeRoles::Peer);

		test::RunTaskTest(context, Num_Expected_Tasks, Peers_Task_Name, [&context, &partnerKey](const auto& task) {
			// Act:
			auto result = task.Callback().get();

			// - wait for success (task completes when pings are started but not yet completed)
			WAIT_FOR_ONE_EXPR(context.counter(Success_Counter_Name));
			WAIT_FOR_ZERO_EXPR(context.counter(Active_Counter_Name));

			// Assert:
			EXPECT_EQ(thread::TaskResult::Continue, result);

			// Assert: subscriber was called and the name from the response node (the Legend) was used
			const auto& subscriber = context.testState().nodeSubscriber();
			ASSERT_EQ(1u, subscriber.nodeParams().params().size());

			const auto& subscriberNode = subscriber.nodeParams().params()[0].NodeCopy;
			EXPECT_EQ(partnerKey, subscriberNode.identityKey());
			EXPECT_EQ("the Legend", subscriberNode.metadata().Name);
		});
	}

	// endregion
}}
