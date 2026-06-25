# Task Management

- [x] Create PacsViewModel for shared connection settings and query results
- [x] Create layouts for fragments:
    - [x] fragment_upload.xml
    - [x] fragment_query.xml
    - [x] fragment_retrieve.xml (with RecyclerView for patient selection)
- [x] Create item_patient.xml for RecyclerView rows
- [x] Create menu for BottomNavigationView
- [x] Implement Fragment classes:
    - [x] UploadFragment (C-STORE) - Scans external storage for .dcm files
    - [x] QueryFragment (C-FIND) - Populates shared ViewModel with PatientRecord objects
    - [x] RetrieveFragment (C-MOVE) - Displays query results in RecyclerView for selection
- [x] Implement PatientAdapter and PatientRecord POJO
- [x] Update FileUtil with helper for external storage copy
- [x] Refactor MainActivity to host fragments and manage connection bar
- [x] Implement connectPACS integration in MainActivity
- [x] Verify data flow between Query and Retrieve fragments
