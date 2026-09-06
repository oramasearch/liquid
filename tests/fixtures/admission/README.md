# Admission fixtures

These Kubernetes `AdmissionReview` v1 documents exercise ordinary, tag-only, and exempt-namespace
Pod requests. The ordinary fixture includes regular, init, and ephemeral container lists so the
process-level webhook test proves that every supported image field is reviewed.
