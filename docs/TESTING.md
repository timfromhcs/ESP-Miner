# Testing Strategy & Automated Validation

## Test Hierarchy
1. **Unit Tests (Firmware C Components):**
   - Located in 	est/ and 	est-ci/
   - Run with: idf.py build test
2. **Frontend Unit Tests (AxeOS):**
   - Located in main/http_server/axe-os
   - Run with: 
pm run test:ci (headless Karma/Jasmine with ChromeHeadlessCI)
3. **Hardware-in-the-Loop Validation:**
   - Physical USB COM3 connection
   - Verified ASIC core enumeration, overt ASICBoost rolling, closed-loop thermal regulation, and Stratum share acceptance.
