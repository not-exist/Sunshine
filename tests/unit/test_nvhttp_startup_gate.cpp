/**
 * @file tests/unit/test_nvhttp_startup_gate.cpp
 * @brief Test the GameStream endpoints that reject requests during startup.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

// lib includes
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/client_https.hpp>

// local includes
#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/video.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Parse a GameStream XML response body.
   *
   * @param body XML response content.
   * @return Parsed property tree.
   */
  boost::property_tree::ptree parse_xml_response(const std::string &body) {
    boost::property_tree::ptree tree;
    std::istringstream data {body};
    boost::property_tree::read_xml(data, tree);
    return tree;
  }

  /**
   * @brief Serve the gated GameStream handlers on an isolated loopback listener.
   */
  struct NvhttpStartupGateTest: BaseTest {
    /**
     * @brief Start an HTTPS server with the production GameStream handlers.
     */
    void SetUp() override {
      BaseTest::SetUp();
      video::set_startup_ready_for_testing(false);

      const auto credentials = crypto::gen_creds("Sunshine startup gate test", 2048);
      certificate_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "nvhttp_startup_gate_cert.pem";
      private_key_file = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "nvhttp_startup_gate_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      server = nvhttp::test_support::make_https_server(certificate_file.string(), private_key_file.string(), handler_scope);
      server->io_service = std::make_shared<boost::asio::io_context>();
      server->resource["^/serverinfo$"]["GET"] = nvhttp::test_support::serverinfo_https;
      server->resource["^/applist$"]["GET"] = nvhttp::test_support::applist_https;
      server->resource["^/launch$"]["GET"] = nvhttp::test_support::launch_https;
      server->resource["^/resume$"]["GET"] = nvhttp::test_support::resume_https;
      server->resource["^/cancel$"]["GET"] = nvhttp::test_support::cancel_https;
      server->config.address = "127.0.0.1";
      server->config.port = 0;

      server_thread = std::jthread([this]() {
        server->start([this](const unsigned short assigned_port) {
          port.store(assigned_port);
        });

        // Depending on the Simple-Web-Server version, start() either runs the event loop or only
        // arms the acceptor; run it here when start() returned.
        if (server->io_service && !server->io_service->stopped()) {
          server->io_service->run();
        }
      });

      for (int i = 0; i < 100 && port.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
      }
      ASSERT_NE(port.load(), 0) << "Server failed to start";

      client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>(std::format("localhost:{}", port.load()), false);
      client->config.timeout = 5;
    }

    /**
     * @brief Stop the server and remove the generated credentials.
     */
    void TearDown() override {
      if (server) {
        server->stop();
      }
      if (server_thread.joinable()) {
        server_thread.join();
      }
      client.reset();
      server.reset();

      std::error_code cleanup_error;
      std::filesystem::remove(certificate_file, cleanup_error);
      std::filesystem::remove(private_key_file, cleanup_error);

      video::set_startup_ready_for_testing(false);
      BaseTest::TearDown();
    }

    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server running the GameStream handlers.
    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> client;  ///< Client used to call the handlers.
    std::shared_ptr<SimpleWeb::ScopeRunner> handler_scope;  ///< Guard used by the server destructor.
    std::jthread server_thread;  ///< Thread running the server event loop.
    std::atomic<unsigned short> port {0};  ///< Assigned loopback port.
    std::filesystem::path certificate_file;  ///< Generated server certificate.
    std::filesystem::path private_key_file;  ///< Generated server private key.
  };

  TEST_F(NvhttpStartupGateTest, ServerInfoReportsNotReady) {
    const auto response = client->request("GET", "/serverinfo", "");

    ASSERT_EQ(response->status_code, "200 OK");
    const auto tree = parse_xml_response(response->content.string());
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Sunshine is still initializing");
  }

  TEST_F(NvhttpStartupGateTest, AppListReportsNotReady) {
    const auto response = client->request("GET", "/applist", "");

    ASSERT_EQ(response->status_code, "200 OK");
    const auto tree = parse_xml_response(response->content.string());
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Sunshine is still initializing");
  }

  TEST_F(NvhttpStartupGateTest, LaunchReportsNotReady) {
    const auto response = client->request("GET", "/launch", "");

    ASSERT_EQ(response->status_code, "200 OK");
    const auto tree = parse_xml_response(response->content.string());
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<int>("root.gamesession"), 0);
  }

  TEST_F(NvhttpStartupGateTest, ResumeReportsNotReady) {
    const auto response = client->request("GET", "/resume", "");

    ASSERT_EQ(response->status_code, "200 OK");
    const auto tree = parse_xml_response(response->content.string());
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<int>("root.resume"), 0);
  }

  TEST_F(NvhttpStartupGateTest, CancelReportsNotReady) {
    const auto response = client->request("GET", "/cancel", "");

    ASSERT_EQ(response->status_code, "200 OK");
    const auto tree = parse_xml_response(response->content.string());
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<int>("root.cancel"), 0);
  }

  TEST(NvhttpStartupGateTreeTest, PopulatesPerEndpointNode) {
    const auto tree = nvhttp::test_support::startup_not_ready_tree_for_testing("root.cancel");

    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Sunshine is still initializing");
    EXPECT_EQ(tree.get<int>("root.cancel"), 0);
  }

  TEST(NvhttpStartupGateTreeTest, OmitsPerEndpointNodeWhenNotRequested) {
    const auto tree = nvhttp::test_support::startup_not_ready_tree_for_testing();

    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_FALSE(tree.get_child_optional("root.cancel"));
  }
}  // namespace
