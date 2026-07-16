# ZOHO-Project

o run a demo scan:
run.bat --demo
For a full list of commands:

run.bat --help
Configuration
Copy config/tiering_config.json.example to config/tiering_config.json.

Configure your storage tiers and policy thresholds within the new file.

Use an .env file (based on .env.example) to manage API keys and database paths.

Project Structure
src/: Core engine logic, policy scorers, API servers, and monitoring.

include/: Header files for the tiering architecture.

tests/: Unit tests utilizing Google Test.

client/: Electron-based dashboard client.
